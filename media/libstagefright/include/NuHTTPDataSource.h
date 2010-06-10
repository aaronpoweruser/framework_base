#ifndef NU_HTTP_DATA_SOURCE_H_

#define NU_HTTP_DATA_SOURCE_H_

#include <media/stagefright/DataSource.h>
#include <utils/String8.h>
#include <utils/threads.h>

#include "HTTPStream.h"

namespace android {

struct NuHTTPDataSource : public DataSource {
    NuHTTPDataSource();

    status_t connect(const char *uri, off_t offset = 0);

    status_t connect(
            const char *host, unsigned port, const char *path,
            off_t offset = 0);

    void disconnect();

    virtual status_t initCheck() const;

    virtual ssize_t readAt(off_t offset, void *data, size_t size);
    virtual status_t getSize(off_t *size);
    virtual uint32_t flags();

protected:
    virtual ~NuHTTPDataSource();

private:
    enum State {
        DISCONNECTED,
        CONNECTING,
        CONNECTED
    };

    Mutex mLock;

    State mState;

    String8 mHost;
    unsigned mPort;
    String8 mPath;

    HTTPStream mHTTP;
    off_t mOffset;
    off_t mContentLength;
    bool mContentLengthValid;

    NuHTTPDataSource(const NuHTTPDataSource &);
    NuHTTPDataSource &operator=(const NuHTTPDataSource &);
};

}  // namespace android

#endif  // NU_HTTP_DATA_SOURCE_H_
