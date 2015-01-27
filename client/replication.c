/* A client for the Postgres logical replication protocol, comparable to
 * pg_recvlogical but adapted to our needs. The protocol is documented here:
 * http://www.postgresql.org/docs/9.4/static/protocol-replication.html */

#include "replication.h"

#include <arpa/inet.h>
#include <sys/time.h>

#include <server/datatype/timestamp.h>
#include <internal/pqexpbuffer.h>

#define CHECKPOINT_INTERVAL_SEC 10


bool parse_keepalive_message(replication_stream_t stream, char *buf, int buflen);
bool parse_xlogdata_message(replication_stream_t stream, char *buf, int buflen);
bool stream_parse_frame_cb(replication_stream_t stream, XLogRecPtr wal_pos, char *buf, int buflen);
int64 current_time(void);
void sendint64(int64 i64, char *buf);
int64 recvint64(char *buf);

bool stream_parse_frame_cb(replication_stream_t stream, XLogRecPtr wal_pos, char *buf, int buflen) {
    for (int i = 0; i < buflen; i++) {
        printf("%02hhx", buf[i]);
    }
    printf("\n");
    return true;
}

bool consume_stream(PGconn *conn, char *slot_name) {
    if (!start_stream(conn, slot_name, InvalidXLogRecPtr)) return false;

    struct replication_stream stream;
    stream.conn = conn;
    stream.frame_cb = stream_parse_frame_cb;
    stream.recvd_lsn = InvalidXLogRecPtr;
    stream.fsync_lsn = InvalidXLogRecPtr;
    stream.last_checkpoint = 0;

    bool success = true;
    while (success) { // TODO while not aborted
        int ret = poll_stream(&stream);

        /* End of stream */
        if (ret == -1) break;

        /* Some error occurred (message has already been logged) */
        if (ret == -2) success = false;

        /* Nothing available to read right now. Wait on the socket until data arrives */
        if (ret == 0) {
            fd_set input_mask;
            FD_ZERO(&input_mask);
            FD_SET(PQsocket(conn), &input_mask);

            struct timeval timeout;
            timeout.tv_sec = 1;
            timeout.tv_usec = 0;

            ret = select(PQsocket(conn) + 1, &input_mask, NULL, NULL, &timeout);

            if (ret == 0 || (ret < 0 && errno == EINTR)) {
                continue; /* timeout or signal */
            } else if (ret < 0) {
                fprintf(stderr, "select() failed: %s\n", strerror(errno));
                success = false;
            }

            /* Data has arrived on the socket */
            if (PQconsumeInput(conn) == 0) {
                fprintf(stderr, "Could not receive data from server: %s", PQerrorMessage(conn));
                success = false;
            }
        }
    }

    PGresult *res = PQgetResult(conn);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        fprintf(stderr, "Replication stream was unexpectedly terminated: %s",
                PQresultErrorMessage(res));
    }
    PQclear(res);
    return success;
}

/* Send a "Standby status update" message to server, indicating the LSN up to which we
 * have received logs. This message is packed binary with the following structure:
 *
 *   - Byte1('r'): Identifies the message as a receiver status update.
 *   - Int64: The location of the last WAL byte + 1 received by the client.
 *   - Int64: The location of the last WAL byte + 1 stored durably by the client.
 *   - Int64: The location of the last WAL byte + 1 applied to the client DB.
 *   - Int64: The client's system clock, as microseconds since midnight on 2000-01-01.
 *   - Byte1: If 1, the client requests the server to reply to this message immediately.
 */
bool checkpoint(replication_stream_t stream, int64 now) {
    char buf[1 + 8 + 8 + 8 + 8 + 1];
    int offset = 0;

    buf[offset] = 'r';                          offset += 1;
    sendint64(stream->recvd_lsn, &buf[offset]); offset += 8;
    sendint64(stream->fsync_lsn, &buf[offset]); offset += 8;
    sendint64(InvalidXLogRecPtr, &buf[offset]); offset += 8; // only used by physical replication
    sendint64(now,               &buf[offset]); offset += 8;
    buf[offset] = 0;                            offset += 1;

    if (PQputCopyData(stream->conn, buf, offset) <= 0 || PQflush(stream->conn)) {
        fprintf(stderr, "Could not send checkpoint to server: %s\n",
                PQerrorMessage(stream->conn));
        return false;
    }

    stream->last_checkpoint = now;
    return true;
}

bool start_stream(PGconn *conn, char *slot_name, XLogRecPtr position) {
    PQExpBuffer query = createPQExpBuffer();
    appendPQExpBuffer(query, "START_REPLICATION SLOT \"%s\" LOGICAL %X/%X",
            slot_name, (uint32) (position >> 32), (uint32) position);

    PGresult *res = PQexec(conn, query->data);

    if (PQresultStatus(res) != PGRES_COPY_BOTH) {
        fprintf(stderr, "Could not send replication command \"%s\": %s\n",
                query->data, PQresultErrorMessage(res));
        PQclear(res);
        goto error;
    }

    PQclear(res);
    destroyPQExpBuffer(query);
    return true;

error:
    destroyPQExpBuffer(query);
    return false;
}


/* Tries to read and process one message from a replication stream, using async I/O.
 * Returns 1 if a message was processed, 0 if there is no data available right now,
 * -1 if the stream has ended, and -2 if an error occurred. */
int poll_stream(replication_stream_t stream) {
    char *buf = NULL;
    int ret = PQgetCopyData(stream->conn, &buf, 1);
    bool success = true;

    if (ret <= 0) {
        if (ret == -2) {
            fprintf(stderr, "Could not read COPY data: %s\n", PQerrorMessage(stream->conn));
        }
        if (buf) PQfreemem(buf);
        return ret;
    }

    switch (buf[0]) {
        case 'k':
            success = parse_keepalive_message(stream, buf, ret);
            break;
        case 'w':
            success = parse_xlogdata_message(stream, buf, ret);
            break;
        default:
            fprintf(stderr, "Unknown streaming message type: \"%c\"\n", buf[0]);
            success = false;
    }

    /* Periodically let the server know up to which point we've consumed the stream. */
    if (success && stream->recvd_lsn != InvalidXLogRecPtr) {
        int64 now = current_time();
        if (now - stream->last_checkpoint > CHECKPOINT_INTERVAL_SEC * USECS_PER_SEC) {
            success = checkpoint(stream, now);
        }
    }

    PQfreemem(buf);
    return success ? 1 : -2;
}


/* Parses a "Primary keepalive message" received from the server. It is packed binary
 * with the following structure:
 *
 *   - Byte1('k'): Identifies the message as a sender keepalive.
 *   - Int64: The current end of WAL on the server.
 *   - Int64: The server's system clock at the time of transmission, as microseconds
 *            since midnight on 2000-01-01.
 *   - Byte1: 1 means that the client should reply to this message as soon as possible,
 *            to avoid a timeout disconnect. 0 otherwise.
 */
bool parse_keepalive_message(replication_stream_t stream, char *buf, int buflen) {
    if (buflen < 1 + 8 + 8 + 1) {
        fprintf(stderr, "Keepalive message too small: %d bytes\n", buflen);
        return false;
    }

    int offset = 1; // start with 1 to skip the initial 'k' byte

    XLogRecPtr wal_pos = recvint64(&buf[offset]); offset += 8;
    /* skip server clock timestamp */             offset += 8;
    bool reply_requested = buf[offset];           offset += 1;

    /* Not 100% sure whether it's semantically correct to update our LSN position here --
     * the keepalive message indicates the latest position on the server, which might not
     * necessarily correspond to the latest position on the client. But this is what
     * pg_recvlogical does, so it's probably ok. */
    stream->recvd_lsn = Max(wal_pos, stream->recvd_lsn);

    if (reply_requested) {
        return checkpoint(stream, current_time());
    }
    return true;
}


/* Parses a XLogData message received from the server. It is packed binary with the
 * following structure:
 *
 *   - Byte1('w'): Identifies the message as replication data.
 *   - Int64: The starting point of the WAL data in this message.
 *   - Int64: The current end of WAL on the server.
 *   - Int64: The server's system clock at the time of transmission, as microseconds
 *            since midnight on 2000-01-01.
 *   - Byte(n): The output from the logical replication output plugin.
 */
bool parse_xlogdata_message(replication_stream_t stream, char *buf, int buflen) {
    int hdrlen = 1 + 8 + 8 + 8;

    if (buflen < hdrlen + 1) {
        fprintf(stderr, "XLogData header too small: %d bytes\n", buflen);
        return false;
    }

    XLogRecPtr wal_pos = recvint64(&buf[1]);

    bool success = stream->frame_cb(stream, wal_pos, buf + hdrlen, buflen - hdrlen);

    stream->recvd_lsn = Max(wal_pos, stream->recvd_lsn);

    return success;
}

/* Returns the current date and time (according to the local system clock) in the
 * representation used by Postgres: microseconds since midnight on 2000-01-01. */
int64 current_time() {
    int64 timestamp;
    struct timeval tv;

    gettimeofday(&tv, NULL);
    timestamp = (int64) tv.tv_sec - ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY);
    timestamp = (timestamp * USECS_PER_SEC) + tv.tv_usec;

    return timestamp;
}

/* Converts an int64 to network byte order. */
void sendint64(int64 i64, char *buf) {
    uint32 i32 = htonl((uint32) (i64 >> 32));
    memcpy(&buf[0], &i32, 4);

    i32 = htonl((uint32) i64);
    memcpy(&buf[4], &i32, 4);
}

/* Converts an int64 from network byte order to native format.  */
int64 recvint64(char *buf) {
    uint32 h32, l32;

    memcpy(&h32, buf, 4);
    memcpy(&l32, buf + 4, 4);

    int64 result = ntohl(h32);
    result <<= 32;
    result |= ntohl(l32);
    return result;
}
