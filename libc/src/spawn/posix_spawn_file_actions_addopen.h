//===-- Impl header for posix_spawn_file_actions_addopen --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SRC_SPAWN_POSIX_SPAWN_FILE_ACTIONS_ADDOPEN_H
#define LLVM_LIBC_SRC_SPAWN_POSIX_SPAWN_FILE_ACTIONS_ADDOPEN_H

#include "src/__support/macros/config.h"
#include <spawn.h>

namespace LIBC_NAMESPACE_DECL {

int posix_spawn_file_actions_addopen(
    posix_spawn_file_actions_t *__restrict actions, int fd,
    const char *__restrict path, int oflag, mode_t mode);

} // namespace LIBC_NAMESPACE_DECL

#endif // LLVM_LIBC_SRC_SPAWN_POSIX_SPAWN_FILE_ACTIONS_ADDOPEN_H
