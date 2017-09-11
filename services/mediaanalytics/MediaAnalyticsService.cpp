/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Proxy for media player implementations

//#define LOG_NDEBUG 0
#define LOG_TAG "MediaAnalyticsService"
#include <utils/Log.h>

#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>

#include <string.h>
#include <pwd.h>

#include <cutils/atomic.h>
#include <cutils/properties.h> // for property_get

#include <utils/misc.h>

#include <android/content/pm/IPackageManagerNative.h>

#include <binder/IPCThreadState.h>
#include <binder/IServiceManager.h>
#include <binder/MemoryHeapBase.h>
#include <binder/MemoryBase.h>
#include <gui/Surface.h>
#include <utils/Errors.h>  // for status_t
#include <utils/List.h>
#include <utils/String8.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>
#include <utils/Vector.h>

#include <media/AudioPolicyHelper.h>
#include <media/IMediaHTTPService.h>
#include <media/IRemoteDisplay.h>
#include <media/IRemoteDisplayClient.h>
#include <media/MediaPlayerInterface.h>
#include <media/mediarecorder.h>
#include <media/MediaMetadataRetrieverInterface.h>
#include <media/Metadata.h>
#include <media/AudioTrack.h>
#include <media/MemoryLeakTrackUtil.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/Utils.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooperRoster.h>
#include <mediautils/BatteryNotifier.h>

//#include <memunreachable/memunreachable.h>
#include <system/audio.h>

#include <private/android_filesystem_config.h>

#include "MediaAnalyticsService.h"

#include "MetricsSummarizer.h"
#include "MetricsSummarizerCodec.h"
#include "MetricsSummarizerExtractor.h"
#include "MetricsSummarizerPlayer.h"
#include "MetricsSummarizerRecorder.h"


namespace android {

    using namespace android::base;
    using namespace android::content::pm;



// summarized records
// up to 36 sets, each covering an hour -- so at least 1.5 days
// (will be longer if there are hours without any media action)
static const nsecs_t kNewSetIntervalNs = 3600*(1000*1000*1000ll);
static const int kMaxRecordSets = 36;

// individual records kept in memory: age or count
// age: <= 36 hours (1.5 days)
// count: hard limit of # records
// (0 for either of these disables that threshold)
static const nsecs_t kMaxRecordAgeNs =  36 * 3600 * (1000*1000*1000ll);
static const int kMaxRecords    = 0;

static const char *kServiceName = "media.metrics";

void MediaAnalyticsService::instantiate() {
    defaultServiceManager()->addService(
            String16(kServiceName), new MediaAnalyticsService());
}

// handle sets of summarizers
MediaAnalyticsService::SummarizerSet::SummarizerSet() {
    mSummarizers = new List<MetricsSummarizer *>();
}

MediaAnalyticsService::SummarizerSet::~SummarizerSet() {
    // empty the list
    List<MetricsSummarizer *> *l = mSummarizers;
    while (l->size() > 0) {
        MetricsSummarizer *summarizer = *(l->begin());
        l->erase(l->begin());
        delete summarizer;
    }
}

void MediaAnalyticsService::newSummarizerSet() {
    ALOGD("MediaAnalyticsService::newSummarizerSet");
    MediaAnalyticsService::SummarizerSet *set = new MediaAnalyticsService::SummarizerSet();
    nsecs_t now = systemTime(SYSTEM_TIME_REALTIME);
    set->setStarted(now);

    set->appendSummarizer(new MetricsSummarizerExtractor("extractor"));
    set->appendSummarizer(new MetricsSummarizerCodec("codec"));
    set->appendSummarizer(new MetricsSummarizerPlayer("nuplayer"));
    set->appendSummarizer(new MetricsSummarizerRecorder("recorder"));

    // ALWAYS at the end, since it catches everything
    set->appendSummarizer(new MetricsSummarizer(NULL));

    // inject this set at the BACK of the list.
    mSummarizerSets->push_back(set);
    mCurrentSet = set;

    // limit the # that we have
    if (mMaxRecordSets > 0) {
        List<SummarizerSet *> *l = mSummarizerSets;
        while (l->size() > (size_t) mMaxRecordSets) {
            ALOGD("Deleting oldest record set....");
            MediaAnalyticsService::SummarizerSet *oset = *(l->begin());
            l->erase(l->begin());
            delete oset;
            mSetsDiscarded++;
        }
    }
}

MediaAnalyticsService::MediaAnalyticsService()
        : mMaxRecords(kMaxRecords),
          mMaxRecordAgeNs(kMaxRecordAgeNs),
          mMaxRecordSets(kMaxRecordSets),
          mNewSetInterval(kNewSetIntervalNs),
          mDumpProto(MediaAnalyticsItem::PROTO_V0) {

    ALOGD("MediaAnalyticsService created");
    // clear our queues
    mOpen = new List<MediaAnalyticsItem *>();
    mFinalized = new List<MediaAnalyticsItem *>();

    mSummarizerSets = new List<MediaAnalyticsService::SummarizerSet *>();
    newSummarizerSet();

    mItemsSubmitted = 0;
    mItemsFinalized = 0;
    mItemsDiscarded = 0;
    mItemsDiscardedExpire = 0;
    mItemsDiscardedCount = 0;

    mLastSessionID = 0;
    // recover any persistency we set up
    // etc
}

MediaAnalyticsService::~MediaAnalyticsService() {
        ALOGD("MediaAnalyticsService destroyed");

    // clean out mOpen and mFinalized
    while (mOpen->size() > 0) {
        MediaAnalyticsItem * oitem = *(mOpen->begin());
        mOpen->erase(mOpen->begin());
        delete oitem;
        mItemsDiscarded++;
        mItemsDiscardedCount++;
    }
    delete mOpen;
    mOpen = NULL;

    while (mFinalized->size() > 0) {
        MediaAnalyticsItem * oitem = *(mFinalized->begin());
        mFinalized->erase(mFinalized->begin());
        delete oitem;
        mItemsDiscarded++;
        mItemsDiscardedCount++;
    }
    delete mFinalized;
    mFinalized = NULL;

    // XXX: clean out the summaries
}


MediaAnalyticsItem::SessionID_t MediaAnalyticsService::generateUniqueSessionID() {
    // generate a new sessionid

    Mutex::Autolock _l(mLock_ids);
    return (++mLastSessionID);
}

// caller surrenders ownership of 'item'
MediaAnalyticsItem::SessionID_t MediaAnalyticsService::submit(MediaAnalyticsItem *item, bool forcenew) {

    MediaAnalyticsItem::SessionID_t id = MediaAnalyticsItem::SessionIDInvalid;

    // we control these, generally not trusting user input
    nsecs_t now = systemTime(SYSTEM_TIME_REALTIME);
    // round nsecs to seconds
    now = ((now + 500000000) / 1000000000) * 1000000000;
    item->setTimestamp(now);
    int pid = IPCThreadState::self()->getCallingPid();
    int uid = IPCThreadState::self()->getCallingUid();

    int uid_given = item->getUid();
    int pid_given = item->getPid();

    // although we do make exceptions for particular client uids
    // that we know we trust.
    //
    bool isTrusted = false;

    ALOGV("caller has uid=%d, embedded uid=%d", uid, uid_given);

    switch (uid)  {
        case AID_MEDIA:
        case AID_MEDIA_CODEC:
        case AID_MEDIA_EX:
        case AID_MEDIA_DRM:
            // trusted source, only override default values
            isTrusted = true;
            if (uid_given == (-1)) {
                item->setUid(uid);
            }
            if (pid_given == (-1)) {
                item->setPid(pid);
            }
            break;
        default:
            isTrusted = false;
            item->setPid(pid);
            item->setUid(uid);
            break;
    }

    // Overwrite package name and version if the caller was untrusted.
    if (!isTrusted) {
      item->setPkgName(getPkgName(item->getUid(), true));
      item->setPkgVersionCode(0);
    } else if (item->getPkgName().empty()) {
      // Only overwrite the package name if it was empty. Trust whatever
      // version code was provided by the trusted caller.
      item->setPkgName(getPkgName(uid, true));
    }

    ALOGV("given uid %d; sanitized uid: %d sanitized pkg: %s "
          "sanitized pkg version: %d",
          uid_given, item->getUid(),
          item->getPkgName().c_str(),
          item->getPkgVersionCode());

    mItemsSubmitted++;

    // validate the record; we discard if we don't like it
    if (contentValid(item, isTrusted) == false) {
        delete item;
        return MediaAnalyticsItem::SessionIDInvalid;
    }


    // if we have a sesisonid in the new record, look to make
    // sure it doesn't appear in the finalized list.
    // XXX: this is for security / DOS prevention.
    // may also require that we persist the unique sessionIDs
    // across boots [instead of within a single boot]


    // match this new record up against records in the open
    // list...
    // if there's a match, merge them together
    // deal with moving the old / merged record into the finalized que

    bool finalizing = item->getFinalized();

    // if finalizing, we'll remove it
    MediaAnalyticsItem *oitem = findItem(mOpen, item, finalizing | forcenew);
    if (oitem != NULL) {
        if (forcenew) {
            // old one gets finalized, then we insert the new one
            // so we'll have 2 records at the end of this.
            // but don't finalize an empty record
            if (oitem->count() == 0) {
                // we're responsible for disposing of the dead record
                delete oitem;
                oitem = NULL;
            } else {
                oitem->setFinalized(true);
                summarize(oitem);
                saveItem(mFinalized, oitem, 0);
            }
            // new record could itself be marked finalized...
            if (finalizing) {
                summarize(item);
                saveItem(mFinalized, item, 0);
                mItemsFinalized++;
            } else {
                saveItem(mOpen, item, 1);
            }
            id = item->getSessionID();
        } else {
            // combine the records, send it to finalized if appropriate
            oitem->merge(item);
            if (finalizing) {
                summarize(oitem);
                saveItem(mFinalized, oitem, 0);
                mItemsFinalized++;
            }
            id = oitem->getSessionID();

            // we're responsible for disposing of the dead record
            delete item;
            item = NULL;
        }
    } else {
        // nothing to merge, save the new record
        id = item->getSessionID();
        if (finalizing) {
            if (item->count() == 0) {
                // drop empty records
                delete item;
                item = NULL;
            } else {
                summarize(item);
                saveItem(mFinalized, item, 0);
                mItemsFinalized++;
            }
        } else {
            saveItem(mOpen, item, 1);
        }
    }
    return id;
}


status_t MediaAnalyticsService::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 512;
    char buffer[SIZE];
    String8 result;

    if (checkCallingPermission(String16("android.permission.DUMP")) == false) {
        snprintf(buffer, SIZE, "Permission Denial: "
                "can't dump MediaAnalyticsService from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
        result.append(buffer);
        write(fd, result.string(), result.size());
        return NO_ERROR;
    }

    // crack any parameters
    String16 summaryOption("-summary");
    bool summary = false;
    String16 protoOption("-proto");
    String16 clearOption("-clear");
    bool clear = false;
    String16 sinceOption("-since");
    nsecs_t ts_since = 0;
    String16 helpOption("-help");
    String16 onlyOption("-only");
    AString only;
    int n = args.size();

    for (int i = 0; i < n; i++) {
        String8 myarg(args[i]);
        if (args[i] == clearOption) {
            clear = true;
        } else if (args[i] == summaryOption) {
            summary = true;
        } else if (args[i] == protoOption) {
            i++;
            if (i < n) {
                String8 value(args[i]);
                int proto = MediaAnalyticsItem::PROTO_V0;       // default to original
                char *endp;
                const char *p = value.string();
                proto = strtol(p, &endp, 10);
                if (endp != p || *endp == '\0') {
                    if (proto < MediaAnalyticsItem::PROTO_FIRST) {
                        proto = MediaAnalyticsItem::PROTO_FIRST;
                    } else if (proto > MediaAnalyticsItem::PROTO_LAST) {
                        proto = MediaAnalyticsItem::PROTO_LAST;
                    }
                    mDumpProto = proto;
                }
            }
        } else if (args[i] == sinceOption) {
            i++;
            if (i < n) {
                String8 value(args[i]);
                char *endp;
                const char *p = value.string();
                ts_since = strtoll(p, &endp, 10);
                if (endp == p || *endp != '\0') {
                    ts_since = 0;
                }
            } else {
                ts_since = 0;
            }
            // command line is milliseconds; internal units are nano-seconds
            ts_since *= 1000*1000;
        } else if (args[i] == onlyOption) {
            i++;
            if (i < n) {
                String8 value(args[i]);
                only = value.string();
            }
        } else if (args[i] == helpOption) {
            result.append("Recognized parameters:\n");
            result.append("-help        this help message\n");
            result.append("-proto X     dump using protocol X (defaults to 1)");
            result.append("-summary     show summary info\n");
            result.append("-clear       clears out saved records\n");
            result.append("-only X      process records for component X\n");
            result.append("-since X     include records since X\n");
            result.append("             (X is milliseconds since the UNIX epoch)\n");
            write(fd, result.string(), result.size());
            return NO_ERROR;
        }
    }

    Mutex::Autolock _l(mLock);

    // we ALWAYS dump this piece
    snprintf(buffer, SIZE, "Dump of the %s process:\n", kServiceName);
    result.append(buffer);

    dumpHeaders(result, ts_since);

    // want exactly 1, to avoid confusing folks that parse the output
    if (summary) {
        dumpSummaries(result, ts_since, only.c_str());
    } else {
        dumpRecent(result, ts_since, only.c_str());
    }


    if (clear) {
        // remove everything from the finalized queue
        while (mFinalized->size() > 0) {
            MediaAnalyticsItem * oitem = *(mFinalized->begin());
            mFinalized->erase(mFinalized->begin());
            delete oitem;
            mItemsDiscarded++;
        }

        // shall we clear the summary data too?

    }

    write(fd, result.string(), result.size());
    return NO_ERROR;
}

// dump headers
void MediaAnalyticsService::dumpHeaders(String8 &result, nsecs_t ts_since) {
    const size_t SIZE = 512;
    char buffer[SIZE];

    snprintf(buffer, SIZE, "Protocol Version: %d\n", mDumpProto);
    result.append(buffer);

    int enabled = MediaAnalyticsItem::isEnabled();
    if (enabled) {
        snprintf(buffer, SIZE, "Metrics gathering: enabled\n");
    } else {
        snprintf(buffer, SIZE, "Metrics gathering: DISABLED via property\n");
    }
    result.append(buffer);

    snprintf(buffer, SIZE,
        "Since Boot: Submissions: %8" PRId64
            " Finalizations: %8" PRId64 "\n",
        mItemsSubmitted, mItemsFinalized);
    result.append(buffer);
    snprintf(buffer, SIZE,
        "Records Discarded: %8" PRId64
            " (by Count: %" PRId64 " by Expiration: %" PRId64 ")\n",
         mItemsDiscarded, mItemsDiscardedCount, mItemsDiscardedExpire);
    result.append(buffer);
    snprintf(buffer, SIZE,
        "Summary Sets Discarded: %" PRId64 "\n", mSetsDiscarded);
    result.append(buffer);
    if (ts_since != 0) {
        snprintf(buffer, SIZE,
            "Dumping Queue entries more recent than: %" PRId64 "\n",
            (int64_t) ts_since);
        result.append(buffer);
    }
}

// dump summary info
void MediaAnalyticsService::dumpSummaries(String8 &result, nsecs_t ts_since, const char *only) {
    const size_t SIZE = 512;
    char buffer[SIZE];
    int slot = 0;

    snprintf(buffer, SIZE, "\nSummarized Metrics:\n");
    result.append(buffer);

    if (only != NULL && *only == '\0') {
        only = NULL;
    }

    // have each of the distillers dump records
    if (mSummarizerSets != NULL) {
        List<SummarizerSet *>::iterator itSet = mSummarizerSets->begin();
        for (; itSet != mSummarizerSets->end(); itSet++) {
            nsecs_t when = (*itSet)->getStarted();
            if (when < ts_since) {
                continue;
            }
            List<MetricsSummarizer *> *list = (*itSet)->getSummarizers();
            List<MetricsSummarizer *>::iterator it = list->begin();
            for (; it != list->end(); it++) {
                if (only != NULL && strcmp(only, (*it)->getKey()) != 0) {
                    ALOGV("Told to omit '%s'", (*it)->getKey());
                }
                AString distilled = (*it)->dumpSummary(slot, only);
                result.append(distilled.c_str());
            }
        }
    }
}

// the recent, detailed queues
void MediaAnalyticsService::dumpRecent(String8 &result, nsecs_t ts_since, const char * only) {
    const size_t SIZE = 512;
    char buffer[SIZE];

    if (only != NULL && *only == '\0') {
        only = NULL;
    }

    // show the recently recorded records
    snprintf(buffer, sizeof(buffer), "\nFinalized Metrics (oldest first):\n");
    result.append(buffer);
    result.append(this->dumpQueue(mFinalized, ts_since, only));

    snprintf(buffer, sizeof(buffer), "\nIn-Progress Metrics (newest first):\n");
    result.append(buffer);
    result.append(this->dumpQueue(mOpen, ts_since, only));

    // show who is connected and injecting records?
    // talk about # records fed to the 'readers'
    // talk about # records we discarded, perhaps "discarded w/o reading" too
}
// caller has locked mLock...
String8 MediaAnalyticsService::dumpQueue(List<MediaAnalyticsItem *> *theList) {
    return dumpQueue(theList, (nsecs_t) 0, NULL);
}

String8 MediaAnalyticsService::dumpQueue(List<MediaAnalyticsItem *> *theList, nsecs_t ts_since, const char * only) {
    String8 result;
    int slot = 0;

    if (theList->empty()) {
            result.append("empty\n");
    } else {
        List<MediaAnalyticsItem *>::iterator it = theList->begin();
        for (; it != theList->end(); it++) {
            nsecs_t when = (*it)->getTimestamp();
            if (when < ts_since) {
                continue;
            }
            if (only != NULL &&
                strcmp(only, (*it)->getKey().c_str()) != 0) {
                ALOGV("Omit '%s', it's not '%s'", (*it)->getKey().c_str(), only);
                continue;
            }
            AString entry = (*it)->toString(mDumpProto);
            result.appendFormat("%5d: %s\n", slot, entry.c_str());
            slot++;
        }
    }

    return result;
}

//
// Our Cheap in-core, non-persistent records management.
// XXX: rewrite this to manage persistence, etc.

// insert appropriately into queue
void MediaAnalyticsService::saveItem(List<MediaAnalyticsItem *> *l, MediaAnalyticsItem * item, int front) {

    Mutex::Autolock _l(mLock);

    // adding at back of queue (fifo order)
    if (front)  {
        l->push_front(item);
    } else {
        l->push_back(item);
    }

    // keep removing old records the front until we're in-bounds (count)
    if (mMaxRecords > 0) {
        while (l->size() > (size_t) mMaxRecords) {
            MediaAnalyticsItem * oitem = *(l->begin());
            l->erase(l->begin());
            delete oitem;
            mItemsDiscarded++;
            mItemsDiscardedCount++;
        }
    }

    // keep removing old records the front until we're in-bounds (count)
    if (mMaxRecordAgeNs > 0) {
        nsecs_t now = systemTime(SYSTEM_TIME_REALTIME);
        while (l->size() > 0) {
            MediaAnalyticsItem * oitem = *(l->begin());
            nsecs_t when = oitem->getTimestamp();
            // careful about timejumps too
            if ((now > when) && (now-when) <= mMaxRecordAgeNs) {
                // this (and the rest) are recent enough to keep
                break;
            }
            l->erase(l->begin());
            delete oitem;
            mItemsDiscarded++;
            mItemsDiscardedExpire++;
        }
    }
}

// are they alike enough that nitem can be folded into oitem?
static bool compatibleItems(MediaAnalyticsItem * oitem, MediaAnalyticsItem * nitem) {

    // general safety
    if (nitem->getUid() != oitem->getUid()) {
        return false;
    }
    if (nitem->getPid() != oitem->getPid()) {
        return false;
    }

    // key -- needs to match
    if (nitem->getKey() == oitem->getKey()) {
        // still in the game.
    } else {
        return false;
    }

    // session id -- empty field in new is allowed
    MediaAnalyticsItem::SessionID_t osession = oitem->getSessionID();
    MediaAnalyticsItem::SessionID_t nsession = nitem->getSessionID();
    if (nsession != osession) {
        // incoming '0' matches value in osession
        if (nsession != 0) {
            return false;
        }
    }

    return true;
}

// find the incomplete record that this will overlay
MediaAnalyticsItem *MediaAnalyticsService::findItem(List<MediaAnalyticsItem*> *theList, MediaAnalyticsItem *nitem, bool removeit) {
    if (nitem == NULL) {
        return NULL;
    }

    MediaAnalyticsItem *item = NULL;

    Mutex::Autolock _l(mLock);

    for (List<MediaAnalyticsItem *>::iterator it = theList->begin();
        it != theList->end(); it++) {
        MediaAnalyticsItem *tmp = (*it);

        if (!compatibleItems(tmp, nitem)) {
            continue;
        }

        // we match! this is the one I want.
        if (removeit) {
            theList->erase(it);
        }
        item = tmp;
        break;
    }
    return item;
}


// delete the indicated record
void MediaAnalyticsService::deleteItem(List<MediaAnalyticsItem *> *l, MediaAnalyticsItem *item) {

    Mutex::Autolock _l(mLock);

    for (List<MediaAnalyticsItem *>::iterator it = l->begin();
        it != l->end(); it++) {
        if ((*it)->getSessionID() != item->getSessionID())
            continue;
        delete *it;
        l->erase(it);
        break;
    }
}

static AString allowedKeys[] =
{
    "codec",
    "extractor"
};

static const int nAllowedKeys = sizeof(allowedKeys) / sizeof(allowedKeys[0]);

// are the contents good
bool MediaAnalyticsService::contentValid(MediaAnalyticsItem *item, bool isTrusted) {

    // untrusted uids can only send us a limited set of keys
    if (isTrusted == false) {
        // restrict to a specific set of keys
        AString key = item->getKey();

        size_t i;
        for(i = 0; i < nAllowedKeys; i++) {
            if (key == allowedKeys[i]) {
                break;
            }
        }
        if (i == nAllowedKeys) {
            ALOGD("Ignoring (key): %s", item->toString().c_str());
            return false;
        }
    }

    // internal consistency

    return true;
}

// are we rate limited, normally false
bool MediaAnalyticsService::rateLimited(MediaAnalyticsItem *) {

    return false;
}

// insert into the appropriate summarizer.
// we make our own copy to save/summarize
void MediaAnalyticsService::summarize(MediaAnalyticsItem *item) {

    ALOGV("MediaAnalyticsService::summarize()");

    if (item == NULL) {
        return;
    }

    nsecs_t now = systemTime(SYSTEM_TIME_REALTIME);
    if (mCurrentSet == NULL
        || (mCurrentSet->getStarted() + mNewSetInterval < now)) {
        newSummarizerSet();
    }

    if (mCurrentSet == NULL) {
        return;
    }

    List<MetricsSummarizer *> *summarizers = mCurrentSet->getSummarizers();
    List<MetricsSummarizer *>::iterator it = summarizers->begin();
    for (; it != summarizers->end(); it++) {
        if ((*it)->isMine(*item)) {
            break;
        }
    }
    if (it == summarizers->end()) {
        ALOGD("no handler for type %s", item->getKey().c_str());
        return;               // no handler
    }

    // invoke the summarizer. summarizer will make whatever copies
    // it wants; the caller retains ownership of item.

    (*it)->handleRecord(item);

}

// mapping uids to package names

// give me the package name, perhaps going to find it
AString MediaAnalyticsService::getPkgName(uid_t uid, bool addIfMissing) {
    ssize_t i = mPkgMappings.indexOfKey(uid);
    if (i >= 0) {
        AString pkg = mPkgMappings.valueAt(i);
        ALOGV("returning pkg '%s' for uid %d", pkg.c_str(), uid);
        return pkg;
    }

    AString pkg;

    if (addIfMissing == false) {
        return pkg;
    }

    struct passwd *pw = getpwuid(uid);
    if (pw) {
        pkg = pw->pw_name;
    } else {
        pkg = "-";
    }

    // find the proper value

    sp<IBinder> binder = NULL;
    sp<IServiceManager> sm = defaultServiceManager();
    if (sm == NULL) {
        ALOGE("defaultServiceManager failed");
    } else {
        binder = sm->getService(String16("package_native"));
        if (binder == NULL) {
            ALOGE("getService package_native failed");
        }
    }

    if (binder != NULL) {
        sp<IPackageManagerNative> package_mgr = interface_cast<IPackageManagerNative>(binder);

        std::vector<int> uids;
        std::vector<std::string> names;

        uids.push_back(uid);

        binder::Status status = package_mgr->getNamesForUids(uids, &names);
        if (!status.isOk()) {
            ALOGE("package_native::getNamesForUids failed: %s",
                  status.exceptionMessage().c_str());
        } else {
            if (!names[0].empty()) {
                pkg = names[0].c_str();
            }
        }
    }

    // XXX determine whether package was side-loaded or from playstore.
    // for privacy, we only list apps loaded from playstore.

    // Sanitize the package name for ":"
    // as an example, we get "shared:android.uid.systemui"
    // replace : with something benign (I'm going to use !)
    if (!pkg.empty()) {
        int n = pkg.size();
        char *p = (char *) pkg.c_str();
        for (int i = 0 ; i < n; i++) {
            if (p[i] == ':') {
                p[i] = '!';
            }
        }
    }

    // add it to the map, to save a subsequent lookup
    if (!pkg.empty()) {
        ALOGV("Adding uid %d pkg '%s'", uid, pkg.c_str());
        mPkgMappings.add(uid, pkg);
    }

    return pkg;
}

} // namespace android
