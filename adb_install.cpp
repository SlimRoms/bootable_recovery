/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>

#include "ui.h"
#include "cutils/properties.h"
#include "install.h"
#include "common.h"
#include "adb_install.h"
extern "C" {
#include "minadbd/fuse_adb_provider.h"
#include "fuse_sideload.h"
}

static RecoveryUI* ui = NULL;
static pthread_t sideload_thread;

static void
set_usb_driver(bool enabled) {
    int fd = open("/sys/class/android_usb/android0/enable", O_WRONLY);
    if (fd < 0) {
        ui->Print("failed to open driver control: %s\n", strerror(errno));
        return;
    }
    if (write(fd, enabled ? "1" : "0", 1) < 0) {
        ui->Print("failed to set driver control: %s\n", strerror(errno));
    }
    if (close(fd) < 0) {
        ui->Print("failed to close driver control: %s\n", strerror(errno));
    }
}

static void
stop_adbd() {
    property_set("ctl.stop", "adbd");
    set_usb_driver(false);
}


static void
maybe_restart_adbd() {
    char value[PROPERTY_VALUE_MAX+1];
    int len = property_get("ro.debuggable", value, NULL);
    if (len == 1 && value[0] == '1') {
        ui->Print("Restarting adbd...\n");
        set_usb_driver(true);
        property_set("ctl.start", "adbd");
    }
}

struct sideload_waiter_data {
    pid_t child;
};

static struct sideload_waiter_data waiter;

void *adb_sideload_thread(void* v) {
    struct sideload_waiter_data* data = (struct sideload_waiter_data*)v;

    int status;
    waitpid(data->child, &status, 0);
    LOGI("sideload process finished\n");

    ui->CancelWaitKey();

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WEXITSTATUS(status) == 3) {
            ui->Print("\nYou need adb 1.0.32 or newer to sideload\nto this device.\n\n");
        } else if (!WIFSIGNALED(status)) {
            ui->Print("\n(adbd status %d)\n", WEXITSTATUS(status));
        }
    }

    LOGI("sideload thread finished\n");
    return NULL;
}

// How long (in seconds) we wait for the host to start sending us a
// package, before timing out.
#define ADB_INSTALL_TIMEOUT 300

void
start_sideload(RecoveryUI* ui_) {
    ui = ui_;

    stop_adbd();
    set_usb_driver(true);

    ui->Print("\n\nNow send the package you want to apply\n"
              "to the device with \"adb sideload <filename>\"...\n");

    if ((waiter.child = fork()) == 0) {
        execl("/sbin/recovery", "recovery", "--adbd", NULL);
        _exit(-1);
    }

    pthread_create(&sideload_thread, NULL, &adb_sideload_thread, &waiter);
}

int
apply_from_adb(int* wipe_cache, const char* install_file) {
    // FUSE_SIDELOAD_HOST_PATHNAME will start to exist once the host
    // connects and starts serving a package.  Poll for its
    // appearance.  (Note that inotify doesn't work with FUSE.)
    int result;
    int status;
    bool waited = false;
    struct stat st;
    for (int i = 0; i < ADB_INSTALL_TIMEOUT; ++i) {
        if (waitpid(waiter.child, &status, WNOHANG) != 0) {
            result = INSTALL_ERROR;
            waited = true;
            break;
        }

        if (stat(FUSE_SIDELOAD_HOST_PATHNAME, &st) != 0) {
            if (errno == ENOENT && i < ADB_INSTALL_TIMEOUT-1) {
                sleep(1);
                continue;
            } else {
                ui->Print("\nTimed out waiting for package.\n\n", strerror(errno));
                result = INSTALL_ERROR;
                kill(waiter.child, SIGKILL);
                break;
            }
        }
        result = install_package(FUSE_SIDELOAD_HOST_PATHNAME, wipe_cache, install_file, false);
        break;
    }

    if (!waited) {
        // Calling stat() on this magic filename signals the minadbd
        // subprocess to shut down.
        stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &st);

        // TODO(dougz): there should be a way to cancel waiting for a
        // package (by pushing some button combo on the device).  For now
        // you just have to 'adb sideload' a file that's not a valid
        // package, like "/dev/null".
        waitpid(waiter.child, &status, 0);
    }

    set_perf_mode(true);

    set_usb_driver(false);
    maybe_restart_adbd();

    // kill the child
    kill(waiter.child, SIGTERM);
    pthread_join(sideload_thread, NULL);
    ui->FlushKeys();

    set_perf_mode(false);

    return result;
}
