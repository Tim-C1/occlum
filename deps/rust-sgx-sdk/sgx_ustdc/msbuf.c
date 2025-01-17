// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License..

#define _LARGEFILE64_SOURCE

#include <sys/types.h>
#include <string.h>
#include <errno.h>

int u_read_shared_buf(const void *base, void *data, size_t count, off64_t offset)
{
    if (base == NULL || data == NULL || count == 0) {
        return EINVAL;
    }

    memcpy((unsigned char *)data, (unsigned char *)base + offset, count);
    return 0;
}

int u_write_shared_buf(void *base, const void *data, size_t count, off64_t offset)
{
    if (base == NULL || data == NULL || count == 0) {
        return EINVAL;
    }

    memcpy((unsigned char *)base + offset, (unsigned char *)data, count);
    return 0;
}
