/*
 * Copyright (C) 2012-2014 The Android Open Source Project
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

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <string>

#include <android-base/stringprintf.h>
#include <cutils/sockets.h>
#include <log/log_properties.h>
#include <private/android_filesystem_config.h>
#include <sysutils/SocketClient.h>

#include "CommandListener.h"
#include "LogCommand.h"
#include "LogUtils.h"

CommandListener::CommandListener(LogBuffer* buf, LogTags* tags, PruneList* prune)
    : FrameworkListener(getLogSocket()), buf_(buf), tags_(tags), prune_(prune) {
    registerCmd(new ClearCmd(this));
    registerCmd(new GetBufSizeCmd(this));
    registerCmd(new SetBufSizeCmd(this));
    registerCmd(new GetBufSizeUsedCmd(this));
    registerCmd(new GetStatisticsCmd(this));
    registerCmd(new SetPruneListCmd(this));
    registerCmd(new GetPruneListCmd(this));
    registerCmd(new GetEventTagCmd(this));
    registerCmd(new ReinitCmd(this));
    registerCmd(new ExitCmd(this));
}

static void setname() {
    static bool name_set;
    if (!name_set) {
        prctl(PR_SET_NAME, "logd.control");
        name_set = true;
    }
}

int CommandListener::ClearCmd::runCommand(SocketClient* cli, int argc,
                                          char** argv) {
    setname();
    uid_t uid = cli->getUid();
    if (clientHasLogCredentials(cli)) {
        uid = AID_ROOT;
    }

    if (argc < 2) {
        cli->sendMsg("Missing Argument");
        return 0;
    }

    int id = atoi(argv[1]);
    if ((id < LOG_ID_MIN) || (LOG_ID_MAX <= id)) {
        cli->sendMsg("Range Error");
        return 0;
    }

    cli->sendMsg(buf()->clear((log_id_t)id, uid) ? "busy" : "success");
    return 0;
}

int CommandListener::GetBufSizeCmd::runCommand(SocketClient* cli, int argc,
                                               char** argv) {
    setname();
    if (argc < 2) {
        cli->sendMsg("Missing Argument");
        return 0;
    }

    int id = atoi(argv[1]);
    if ((id < LOG_ID_MIN) || (LOG_ID_MAX <= id)) {
        cli->sendMsg("Range Error");
        return 0;
    }

    unsigned long size = buf()->getSize((log_id_t)id);
    char buf[512];
    snprintf(buf, sizeof(buf), "%lu", size);
    cli->sendMsg(buf);
    return 0;
}

int CommandListener::SetBufSizeCmd::runCommand(SocketClient* cli, int argc,
                                               char** argv) {
    setname();
    if (!clientHasLogCredentials(cli)) {
        cli->sendMsg("Permission Denied");
        return 0;
    }

    if (argc < 3) {
        cli->sendMsg("Missing Argument");
        return 0;
    }

    int id = atoi(argv[1]);
    if ((id < LOG_ID_MIN) || (LOG_ID_MAX <= id)) {
        cli->sendMsg("Range Error");
        return 0;
    }

    unsigned long size = atol(argv[2]);
    if (buf()->setSize((log_id_t)id, size)) {
        cli->sendMsg("Range Error");
        return 0;
    }

    cli->sendMsg("success");
    return 0;
}

int CommandListener::GetBufSizeUsedCmd::runCommand(SocketClient* cli, int argc,
                                                   char** argv) {
    setname();
    if (argc < 2) {
        cli->sendMsg("Missing Argument");
        return 0;
    }

    int id = atoi(argv[1]);
    if ((id < LOG_ID_MIN) || (LOG_ID_MAX <= id)) {
        cli->sendMsg("Range Error");
        return 0;
    }

    unsigned long size = buf()->getSizeUsed((log_id_t)id);
    char buf[512];
    snprintf(buf, sizeof(buf), "%lu", size);
    cli->sendMsg(buf);
    return 0;
}

// This returns a string with a length prefix with the format <length>\n<data>\n\f.  The length
// prefix includes the length of the prefix itself.
static std::string PackageString(const std::string& str) {
    size_t overhead_length = 3;  // \n \n \f.

    // Number of digits needed to represent length(str + overhead_length).
    size_t str_size_digits = 1 + static_cast<size_t>(log10(str.size() + overhead_length));
    // Number of digits needed to represent the total size.
    size_t total_size_digits =
            1 + static_cast<size_t>(log10(str.size() + overhead_length + str_size_digits));

    // If adding the size prefix causes a new digit to be required to represent the new total
    // size, add it to the 'overhead_length'.  This can only happen once, since each new digit
    // allows for 10x the previous size to be recorded.
    if (total_size_digits != str_size_digits) {
        overhead_length++;
    }

    size_t total_size = str.size() + overhead_length + str_size_digits;
    return android::base::StringPrintf("%zu\n%s\n\f", total_size, str.c_str());
}

int CommandListener::GetStatisticsCmd::runCommand(SocketClient* cli, int argc,
                                                  char** argv) {
    setname();
    uid_t uid = cli->getUid();
    if (clientHasLogCredentials(cli)) {
        uid = AID_ROOT;
    }

    unsigned int logMask = -1;
    pid_t pid = 0;
    if (argc > 1) {
        logMask = 0;
        for (int i = 1; i < argc; ++i) {
            static const char _pid[] = "pid=";
            if (!strncmp(argv[i], _pid, sizeof(_pid) - 1)) {
                pid = atol(argv[i] + sizeof(_pid) - 1);
                if (pid == 0) {
                    cli->sendMsg("PID Error");
                    return 0;
                }
                continue;
            }

            int id = atoi(argv[i]);
            if ((id < LOG_ID_MIN) || (LOG_ID_MAX <= id)) {
                cli->sendMsg("Range Error");
                return 0;
            }
            logMask |= 1 << id;
        }
    }

    cli->sendMsg(PackageString(buf()->formatStatistics(uid, pid, logMask)).c_str());
    return 0;
}

int CommandListener::GetPruneListCmd::runCommand(SocketClient* cli,
                                                 int /*argc*/, char** /*argv*/) {
    setname();
    cli->sendMsg(PackageString(prune()->format()).c_str());
    return 0;
}

int CommandListener::SetPruneListCmd::runCommand(SocketClient* cli, int argc,
                                                 char** argv) {
    setname();
    if (!clientHasLogCredentials(cli)) {
        cli->sendMsg("Permission Denied");
        return 0;
    }

    std::string str;
    for (int i = 1; i < argc; ++i) {
        if (str.length()) {
            str += " ";
        }
        str += argv[i];
    }

    int ret = prune()->init(str.c_str());

    if (ret) {
        cli->sendMsg("Invalid");
        return 0;
    }

    cli->sendMsg("success");

    return 0;
}

int CommandListener::GetEventTagCmd::runCommand(SocketClient* cli, int argc,
                                                char** argv) {
    setname();
    uid_t uid = cli->getUid();
    if (clientHasLogCredentials(cli)) {
        uid = AID_ROOT;
    }

    const char* name = nullptr;
    const char* format = nullptr;
    const char* id = nullptr;
    for (int i = 1; i < argc; ++i) {
        static const char _name[] = "name=";
        if (!strncmp(argv[i], _name, strlen(_name))) {
            name = argv[i] + strlen(_name);
            continue;
        }

        static const char _format[] = "format=";
        if (!strncmp(argv[i], _format, strlen(_format))) {
            format = argv[i] + strlen(_format);
            continue;
        }

        static const char _id[] = "id=";
        if (!strncmp(argv[i], _id, strlen(_id))) {
            id = argv[i] + strlen(_id);
            continue;
        }
    }

    if (id) {
        if (format || name) {
            cli->sendMsg("can not mix id= with either format= or name=");
            return 0;
        }
        cli->sendMsg(PackageString(tags()->formatEntry(atoi(id), uid)).c_str());
        return 0;
    }

    cli->sendMsg(PackageString(tags()->formatGetEventTag(uid, name, format)).c_str());

    return 0;
}

int CommandListener::ReinitCmd::runCommand(SocketClient* cli, int /*argc*/,
                                           char** /*argv*/) {
    setname();

    android::prdebug("logd reinit");
    buf()->init();
    prune()->init(nullptr);

    // This only works on userdebug and eng devices to re-read the
    // /data/misc/logd/event-log-tags file right after /data is mounted.
    // The operation is near to boot and should only happen once.  There
    // are races associated with its use since it can trigger a Rebuild
    // of the file, but that is a can-not-happen since the file was not
    // read yet.  More dangerous if called later, but if all is well it
    // should just skip over everything and not write any new entries.
    if (__android_log_is_debuggable()) {
        tags()->ReadFileEventLogTags(tags()->debug_event_log_tags);
    }

    cli->sendMsg("success");

    return 0;
}

int CommandListener::ExitCmd::runCommand(SocketClient* cli, int /*argc*/,
                                         char** /*argv*/) {
    setname();

    cli->sendMsg("success");
    parent_->release(cli);

    return 0;
}

int CommandListener::getLogSocket() {
    static const char socketName[] = "logd";
    int sock = android_get_control_socket(socketName);

    if (sock < 0) {
        sock = socket_local_server(
            socketName, ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM);
    }

    return sock;
}
