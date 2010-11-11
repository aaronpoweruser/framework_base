/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <stdint.h>
#include <sys/types.h>

#include <utils/SortedVector.h>
#include <utils/KeyedVector.h>
#include <utils/threads.h>
#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/Singleton.h>
#include <utils/String16.h>

#include <binder/BinderService.h>
#include <binder/IServiceManager.h>

#include <gui/ISensorServer.h>
#include <gui/ISensorEventConnection.h>

#include <hardware/sensors.h>

#include "SensorService.h"

namespace android {
// ---------------------------------------------------------------------------

class BatteryService : public Singleton<BatteryService> {
    static const int TRANSACTION_noteStartSensor = IBinder::FIRST_CALL_TRANSACTION + 3;
    static const int TRANSACTION_noteStopSensor = IBinder::FIRST_CALL_TRANSACTION + 4;
    static const String16 DESCRIPTOR;

    friend class Singleton<BatteryService>;
    sp<IBinder> mBatteryStatService;

    BatteryService() {
        const sp<IServiceManager> sm(defaultServiceManager());
        if (sm != NULL) {
            const String16 name("batteryinfo");
            mBatteryStatService = sm->getService(name);
        }
    }

    status_t noteStartSensor(int uid, int handle) {
        Parcel data, reply;
        data.writeInterfaceToken(DESCRIPTOR);
        data.writeInt32(uid);
        data.writeInt32(handle);
        status_t err = mBatteryStatService->transact(
                TRANSACTION_noteStartSensor, data, &reply, 0);
        err = reply.readExceptionCode();
        return err;
    }

    status_t noteStopSensor(int uid, int handle) {
        Parcel data, reply;
        data.writeInterfaceToken(DESCRIPTOR);
        data.writeInt32(uid);
        data.writeInt32(handle);
        status_t err = mBatteryStatService->transact(
                TRANSACTION_noteStopSensor, data, &reply, 0);
        err = reply.readExceptionCode();
        return err;
    }

public:
    void enableSensor(int handle) {
        if (mBatteryStatService != 0) {
            int uid = IPCThreadState::self()->getCallingUid();
            int64_t identity = IPCThreadState::self()->clearCallingIdentity();
            noteStartSensor(uid, handle);
            IPCThreadState::self()->restoreCallingIdentity(identity);
        }
    }
    void disableSensor(int handle) {
        if (mBatteryStatService != 0) {
            int uid = IPCThreadState::self()->getCallingUid();
            int64_t identity = IPCThreadState::self()->clearCallingIdentity();
            noteStopSensor(uid, handle);
            IPCThreadState::self()->restoreCallingIdentity(identity);
        }
    }
};

const String16 BatteryService::DESCRIPTOR("com.android.internal.app.IBatteryStats");

ANDROID_SINGLETON_STATIC_INSTANCE(BatteryService)

// ---------------------------------------------------------------------------

// 100 events/s max
static const nsecs_t MINIMUM_EVENT_PERIOD = ms2ns(10);

SensorService::SensorService()
    : Thread(false),
      mSensorDevice(0),
      mSensorModule(0),
      mDump("android.permission.DUMP"),
      mInitCheck(NO_INIT)
{
}

void SensorService::onFirstRef()
{
    LOGD("nuSensorService starting...");

    status_t err = hw_get_module(SENSORS_HARDWARE_MODULE_ID,
            (hw_module_t const**)&mSensorModule);

    LOGE_IF(err, "couldn't load %s module (%s)",
            SENSORS_HARDWARE_MODULE_ID, strerror(-err));

    if (mSensorModule) {
        err = sensors_open(&mSensorModule->common, &mSensorDevice);

        LOGE_IF(err, "couldn't open device for module %s (%s)",
                SENSORS_HARDWARE_MODULE_ID, strerror(-err));

        sensors_event_t event;
        memset(&event, 0, sizeof(event));

        struct sensor_t const* list;
        int count = mSensorModule->get_sensors_list(mSensorModule, &list);
        mLastEventSeen.setCapacity(count);
        for (int i=0 ; i<count ; i++) {
            Sensor sensor(list + i);
            LOGI("%s", sensor.getName().string());
            mSensorList.add(sensor);
            if (mSensorDevice) {
                mSensorDevice->activate(mSensorDevice, sensor.getHandle(), 0);
            }
            mLastEventSeen.add(sensor.getHandle(), event);
        }

        if (mSensorDevice) {
            run("SensorService", PRIORITY_URGENT_DISPLAY);
            mInitCheck = NO_ERROR;
        }
    }
}

SensorService::~SensorService()
{
}

status_t SensorService::dump(int fd, const Vector<String16>& args)
{
    const size_t SIZE = 1024;
    char buffer[SIZE];
    String8 result;
    if (!mDump.checkCalling()) {
        snprintf(buffer, SIZE, "Permission Denial: "
                "can't dump SurfaceFlinger from pid=%d, uid=%d\n",
                IPCThreadState::self()->getCallingPid(),
                IPCThreadState::self()->getCallingUid());
        result.append(buffer);
    } else {
        Mutex::Autolock _l(mLock);
        snprintf(buffer, SIZE, "Sensor List:\n");
        result.append(buffer);
        for (size_t i=0 ; i<mSensorList.size() ; i++) {
            const Sensor& s(mSensorList[i]);
            const sensors_event_t& e(mLastEventSeen.valueFor(s.getHandle()));
            snprintf(buffer, SIZE, "%s (vendor=%s, handle=%d, maxRate=%.2fHz, last=<%5.1f,%5.1f,%5.1f>)\n",
                    s.getName().string(),
                    s.getVendor().string(),
                    s.getHandle(),
                    s.getMinDelay() ? (1000000.0f / s.getMinDelay()) : 0.0f,
                    e.data[0], e.data[1], e.data[2]);
            result.append(buffer);
        }

        snprintf(buffer, SIZE, "%d active connections\n",
                mActiveConnections.size());
        result.append(buffer);
        snprintf(buffer, SIZE, "Active sensors:\n");
        result.append(buffer);
        for (size_t i=0 ; i<mActiveSensors.size() ; i++) {
            int handle = mActiveSensors.keyAt(i);
            snprintf(buffer, SIZE, "%s (handle=%d, connections=%d)\n",
                    getSensorName(handle).string(),
                    handle,
                    mActiveSensors.valueAt(i)->getNumConnections());
            result.append(buffer);
        }
    }
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

bool SensorService::threadLoop()
{
    LOGD("nuSensorService thread starting...");

    sensors_event_t buffer[16];
    sensors_event_t scratch[16];
    struct sensors_poll_device_t* device = mSensorDevice;
    ssize_t count;

    do {
        count = device->poll(device, buffer, sizeof(buffer)/sizeof(*buffer));
        if (count<0) {
            LOGE("sensor poll failed (%s)", strerror(-count));
            break;
        }

        recordLastValue(buffer, count);

        const SortedVector< wp<SensorEventConnection> > activeConnections(
                getActiveConnections());

        size_t numConnections = activeConnections.size();
        if (numConnections) {
            for (size_t i=0 ; i<numConnections ; i++) {
                sp<SensorEventConnection> connection(activeConnections[i].promote());
                if (connection != 0) {
                    connection->sendEvents(buffer, count, scratch);
                }
            }
        }

    } while (count >= 0 || Thread::exitPending());

    LOGW("Exiting SensorService::threadLoop!");
    return false;
}

void SensorService::recordLastValue(
        sensors_event_t const * buffer, size_t count)
{
    Mutex::Autolock _l(mLock);

    // record the last event for each sensor
    int32_t prev = buffer[0].sensor;
    for (size_t i=1 ; i<count ; i++) {
        // record the last event of each sensor type in this buffer
        int32_t curr = buffer[i].sensor;
        if (curr != prev) {
            mLastEventSeen.editValueFor(prev) = buffer[i-1];
            prev = curr;
        }
    }
    mLastEventSeen.editValueFor(prev) = buffer[count-1];
}

SortedVector< wp<SensorService::SensorEventConnection> >
SensorService::getActiveConnections() const
{
    Mutex::Autolock _l(mLock);
    return mActiveConnections;
}

String8 SensorService::getSensorName(int handle) const {
    size_t count = mSensorList.size();
    for (size_t i=0 ; i<count ; i++) {
        const Sensor& sensor(mSensorList[i]);
        if (sensor.getHandle() == handle) {
            return sensor.getName();
        }
    }
    String8 result("unknown");
    return result;
}

Vector<Sensor> SensorService::getSensorList()
{
    return mSensorList;
}

sp<ISensorEventConnection> SensorService::createSensorEventConnection()
{
    sp<SensorEventConnection> result(new SensorEventConnection(this));
    return result;
}

void SensorService::cleanupConnection(const wp<SensorEventConnection>& connection)
{
    Mutex::Autolock _l(mLock);
    size_t size = mActiveSensors.size();
    for (size_t i=0 ; i<size ; ) {
        SensorRecord* rec = mActiveSensors.valueAt(i);
        if (rec && rec->removeConnection(connection)) {
            mSensorDevice->activate(mSensorDevice, mActiveSensors.keyAt(i), 0);
            mActiveSensors.removeItemsAt(i, 1);
            delete rec;
            size--;
        } else {
            i++;
        }
    }
    mActiveConnections.remove(connection);
}

status_t SensorService::enable(const sp<SensorEventConnection>& connection,
        int handle)
{
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    status_t err = NO_ERROR;
    Mutex::Autolock _l(mLock);
    SensorRecord* rec = mActiveSensors.valueFor(handle);
    if (rec == 0) {
        rec = new SensorRecord(connection);
        mActiveSensors.add(handle, rec);
        err = mSensorDevice->activate(mSensorDevice, handle, 1);
        LOGE_IF(err, "Error activating sensor %d (%s)", handle, strerror(-err));
        if (err == 0) {
            BatteryService::getInstance().enableSensor(handle);
        }
    } else {
        if (rec->addConnection(connection)) {
            // this sensor is already activated, but we are adding a
            // connection that uses it. Immediately send down the last
            // known value of the requested sensor.
            sensors_event_t scratch;
            sensors_event_t& event(mLastEventSeen.editValueFor(handle));
            if (event.version == sizeof(sensors_event_t)) {
                connection->sendEvents(&event, 1);
            }
        }
    }
    if (err == NO_ERROR) {
        // connection now active
        if (connection->addSensor(handle)) {
            // the sensor was added (which means it wasn't already there)
            // so, see if this connection becomes active
            if (mActiveConnections.indexOf(connection) < 0) {
                mActiveConnections.add(connection);
            }
            // this could change the sensor event delivery speed
            recomputeEventsPeriodLocked(handle);
        }
    }
    return err;
}

status_t SensorService::disable(const sp<SensorEventConnection>& connection,
        int handle)
{
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    status_t err = NO_ERROR;
    Mutex::Autolock _l(mLock);
    SensorRecord* rec = mActiveSensors.valueFor(handle);
    if (rec) {
        // see if this connection becomes inactive
        connection->removeSensor(handle);
        if (connection->hasAnySensor() == false) {
            mActiveConnections.remove(connection);
        }
        // see if this sensor becomes inactive
        if (rec->removeConnection(connection)) {
            mActiveSensors.removeItem(handle);
            delete rec;
            err = mSensorDevice->activate(mSensorDevice, handle, 0);
            if (err == 0) {
                BatteryService::getInstance().disableSensor(handle);
            }
        }
    }
    if (err == NO_ERROR) {
        recomputeEventsPeriodLocked(handle);
    }
    return err;
}

status_t SensorService::setEventRate(const sp<SensorEventConnection>& connection,
        int handle, nsecs_t ns)
{
    if (mInitCheck != NO_ERROR)
        return mInitCheck;

    if (ns < 0)
        return BAD_VALUE;

    if (ns < MINIMUM_EVENTS_PERIOD)
        ns = MINIMUM_EVENTS_PERIOD;

    Mutex::Autolock _l(mLock);
    status_t err = connection->setEventRateLocked(handle, ns);
    if (err == NO_ERROR) {
        recomputeEventsPeriodLocked(handle);
    }
    return err;
}

status_t SensorService::recomputeEventsPeriodLocked(int32_t handle)
{
    status_t err = NO_ERROR;
    nsecs_t wanted = ms2ns(1000);
    size_t count = mActiveConnections.size();
    for (size_t i=0 ; i<count ; i++) {
        sp<SensorEventConnection> connection(mActiveConnections[i].promote());
        if (connection != NULL) {
            nsecs_t ns = connection->getEventRateForSensor(handle);
            if (ns) {
                wanted = wanted < ns ? wanted : ns;
            }
        }
    }
    err = mSensorDevice->setDelay(mSensorDevice, handle, wanted);
    return err;
}

// ---------------------------------------------------------------------------

SensorService::SensorRecord::SensorRecord(
        const sp<SensorEventConnection>& connection)
{
    mConnections.add(connection);
}

bool SensorService::SensorRecord::addConnection(
        const sp<SensorEventConnection>& connection)
{
    if (mConnections.indexOf(connection) < 0) {
        mConnections.add(connection);
        return true;
    }
    return false;
}

bool SensorService::SensorRecord::removeConnection(
        const wp<SensorEventConnection>& connection)
{
    ssize_t index = mConnections.indexOf(connection);
    if (index >= 0) {
        mConnections.removeItemsAt(index, 1);
    }
    return mConnections.size() ? false : true;
}

// ---------------------------------------------------------------------------

SensorService::SensorEventConnection::SensorEventConnection(
        const sp<SensorService>& service)
    : mService(service), mChannel(new SensorChannel())
{
}

SensorService::SensorEventConnection::~SensorEventConnection()
{
    mService->cleanupConnection(this);
}

void SensorService::SensorEventConnection::onFirstRef()
{
}

bool SensorService::SensorEventConnection::addSensor(int32_t handle) {
    if (mSensorInfo.indexOfKey(handle) <= 0) {
        SensorInfo info;
        mSensorInfo.add(handle, info);
        return true;
    }
    return false;
}

bool SensorService::SensorEventConnection::removeSensor(int32_t handle) {
    if (mSensorInfo.removeItem(handle) >= 0) {
        return true;
    }
    return false;
}

bool SensorService::SensorEventConnection::hasSensor(int32_t handle) const {
    return mSensorInfo.indexOfKey(handle) >= 0;
}

bool SensorService::SensorEventConnection::hasAnySensor() const {
    return mSensorInfo.size() ? true : false;
}

status_t SensorService::SensorEventConnection::setEventRateLocked(
        int handle, nsecs_t ns)
{
    ssize_t index = mSensorInfo.indexOfKey(handle);
    if (index >= 0) {
        SensorInfo& info = mSensorInfo.editValueFor(handle);
        info.ns = ns;
        return NO_ERROR;
    }
    return status_t(index);
}

status_t SensorService::SensorEventConnection::sendEvents(
        sensors_event_t const* buffer, size_t numEvents,
        sensors_event_t* scratch)
{
    // filter out events not for this connection
    size_t count = 0;
    if (scratch) {
        size_t i=0;
        while (i<numEvents) {
            const int32_t curr = buffer[i].sensor;
            if (mSensorInfo.indexOfKey(curr) >= 0) {
                do {
                    scratch[count++] = buffer[i++];
                } while ((i<numEvents) && (buffer[i].sensor == curr));
            } else {
                i++;
            }
        }
    } else {
        scratch = const_cast<sensors_event_t *>(buffer);
        count = numEvents;
    }

    if (count == 0)
        return 0;

    ssize_t size = mChannel->write(scratch, count*sizeof(sensors_event_t));
    if (size == -EAGAIN) {
        // the destination doesn't accept events anymore, it's probably
        // full. For now, we just drop the events on the floor.
        LOGW("dropping %d events on the floor", count);
        return size;
    }

    LOGE_IF(size<0, "dropping %d events on the floor (%s)",
            count, strerror(-size));

    return size < 0 ? status_t(size) : status_t(NO_ERROR);
}

sp<SensorChannel> SensorService::SensorEventConnection::getSensorChannel() const
{
    return mChannel;
}

status_t SensorService::SensorEventConnection::enableDisable(
        int handle, bool enabled)
{
    status_t err;
    if (enabled) {
        err = mService->enable(this, handle);
    } else {
        err = mService->disable(this, handle);
    }
    return err;
}

status_t SensorService::SensorEventConnection::setEventRate(
        int handle, nsecs_t ns)
{
    return mService->setEventRate(this, handle, ns);
}

// ---------------------------------------------------------------------------
}; // namespace android

