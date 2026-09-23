// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * GeekMagic Open Firmware
 * Copyright (C) 2026 Times-Z
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

enum LogLevel { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR };

// TLS/JSON 需要大量 heap，环形日志缓冲从 20*96=1920B 削到 4*64=256B；
// LOG_MIN_LEVEL=LOG_ERROR 时实际常驻仅按需分配一次 256B，保留错误日志与 API 读取能力
static constexpr LogLevel LOG_MIN_LEVEL = LOG_ERROR;
static constexpr size_t LOG_BUFFER_MAX_ENTRIES = 4;
static constexpr size_t LOG_ENTRY_MAX_LEN = 64;

class Logger {
   public:
    static void log(LogLevel level, const char* message, const char* className = nullptr);
    static void debug(const char* message, const char* className = nullptr);
    static void info(const char* message, const char* className = nullptr);
    static void warn(const char* message, const char* className = nullptr);
    static void error(const char* message, const char* className = nullptr);

    static String getLogsAsString();
    static size_t getLogCount();
    static const char* getLogEntry(size_t index);
    static void clearLogs();

   private:
    static void printTime();
    static const char* levelToString(LogLevel level);
    static void addToBuffer(const char* entry);
    static void ensureBufferAllocated();

    static char* _logBuffer;
    static size_t _head;
    static size_t _count;
};

#endif  // LOGGER_H
