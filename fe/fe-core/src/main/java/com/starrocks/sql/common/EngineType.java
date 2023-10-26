// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package com.starrocks.sql.common;

import com.google.common.collect.ImmutableSet;

import java.util.Set;

public enum EngineType {
    OLAP,
    MYSQL,
    BROKER,
    ELASTICSEARCH,
    HIVE,
    ICEBERG,
    HUDI,
    JDBC,
    ODPS,
    FILE;

    public static Set<EngineType> SUPPORT_NOT_NULL_SET = ImmutableSet.of(
            OLAP,
            MYSQL,
            BROKER
    );

    public static EngineType defaultEngine() {
        return OLAP;
    }

    public static boolean supportNotNullColumn(String engineName) {
        return SUPPORT_NOT_NULL_SET.contains(EngineType.valueOf(engineName.toUpperCase()));
    }
}
