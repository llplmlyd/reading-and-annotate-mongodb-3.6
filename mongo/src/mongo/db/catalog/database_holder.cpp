/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database_holder.h"

namespace mongo {

DatabaseHolder::Impl::~Impl() = default;

namespace {
stdx::function<DatabaseHolder::factory_function_type> factory;
}  // namespace

void DatabaseHolder::registerFactory(decltype(factory) newFactory) {
    factory = std::move(newFactory);
}

auto DatabaseHolder::makeImpl() -> std::unique_ptr<Impl> {
    return factory();
}

void DatabaseHolder::TUHook::hook() noexcept {}

namespace {
stdx::function<decltype(dbHolder)> dbHolderImpl;
}  // namespace
}  // namespace mongo

// The `mongo::` prefix is necessary to placate MSVC -- it is unable to properly identify anonymous
// nested namespace members in `decltype` expressions when defining functions using scope-resolution
// syntax.

void mongo::registerDbHolderImpl(decltype(mongo::dbHolderImpl) impl) {
	//InitializeDbHolderimpl中赋值
	//下面的mongo::dbHolder()运行
    dbHolderImpl = std::move(impl);
}

//AutoGetDb::AutoGetDb中调用
//AutoGetDb::AutoGetDb 或者 AutoGetOrCreateDb::AutoGetOrCreateDb调用
auto mongo::dbHolder() -> DatabaseHolder& {
	//也就是mongo::registerDbHolderImpl注册的impl，也就是database_holder_impl.h中的全局变量_dbHolder
    return dbHolderImpl();
}

