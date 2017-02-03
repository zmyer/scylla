/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright (C) 2015 ScyllaDB
 *
 * Modified by ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "operator.hh"

namespace cql3 {

const operator_type operator_type::EQ(0, operator_type::EQ, "=");
const operator_type operator_type::LT(4, operator_type::GT, "<");
const operator_type operator_type::LTE(3, operator_type::GTE, "<=");
const operator_type operator_type::GTE(1, operator_type::LTE, ">=");
const operator_type operator_type::GT(2, operator_type::LT, ">");
const operator_type operator_type::IN(7, operator_type::IN, "IN");
const operator_type operator_type::CONTAINS(5, operator_type::CONTAINS, "CONTAINS");
const operator_type operator_type::CONTAINS_KEY(6, operator_type::CONTAINS_KEY, "CONTAINS_KEY");
const operator_type operator_type::NEQ(8, operator_type::NEQ, "!=");
const operator_type operator_type::IS_NOT(9, operator_type::IS_NOT, "IS NOT");

}
