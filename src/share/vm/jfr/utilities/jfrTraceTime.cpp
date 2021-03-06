/*
 * Copyright (c) 2013, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jfr/utilities/jfrTraceTime.hpp"
#include "runtime/os.hpp"
#ifdef X86
#include "rdtsc_x86.hpp"
#endif

#ifdef TARGET_OS_FAMILY_linux
# include "os_linux.inline.hpp"
# include "os_posix.hpp"
#endif
#ifdef TARGET_OS_FAMILY_solaris
# include "os_solaris.inline.hpp"
# include "os_posix.hpp"
#endif
#ifdef TARGET_OS_FAMILY_windows
# include "os_windows.inline.hpp"
#endif
#ifdef TARGET_OS_FAMILY_aix
# include "os_aix.inline.hpp"
# include "os_posix.hpp"
#endif
#ifdef TARGET_OS_FAMILY_bsd
# include "os_posix.hpp"
# include "os_bsd.inline.hpp"
#endif

bool JfrTraceTime::_ft_enabled = false;

bool JfrTraceTime::initialize() {
  static bool initialized = false;
  if (!initialized) {
#ifdef X86
    _ft_enabled = Rdtsc::initialize();
#else
    _ft_enabled = false;
#endif
    initialized = true;
  }
  return initialized;
}

bool JfrTraceTime::is_ft_supported() {
#ifdef X86
  return Rdtsc::is_supported();
#else
  return false;
#endif
}

JfrTraceTime::JfrTraceTime() {
#ifdef X86
  _ticks = _ft_enabled ? Rdtsc::elapsed_counter() : os::elapsed_counter();
#else
  _ticks = os::elapsed_counter();
#endif
}

JfrTraceTime JfrTraceTime::now() {
  return JfrTraceTime();
}

void JfrTraceTime::stamp() {
  _ticks = now().value();
}

const void* JfrTraceTime::time_function() {
#ifdef X86
  return _ft_enabled ? (const void*)Rdtsc::elapsed_counter : (const void*)os::elapsed_counter;
#else
  return (const void*)os::elapsed_counter;
#endif
}

jlong JfrTraceTime::frequency() {
#ifdef X86
  return _ft_enabled ? Rdtsc::frequency() : os::elapsed_frequency();
#else
  return os::elapsed_frequency();
#endif
}

