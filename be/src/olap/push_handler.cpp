// Copyright (c) 2017, Baidu.com, Inc. All Rights Reserved

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "olap/push_handler.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include <boost/filesystem.hpp>

#include "olap/olap_engine.h"
#include "olap/olap_table.h"
#include "olap/schema_change.h"

using std::list;
using std::map;
using std::string;
using std::vector;

namespace palo {

// Process push command, the main logical is as follows:
//    a. related tables not exist:
//        current table isn't in schemachange state, only push for current table
//    b. related tables exist
//       I.  current table is old table:
//           push for current table and than convert data for related tables
//       II. current table is new table:
//           this usually means schema change is over,
//           clear schema change info in both current table and related tables,
//           finally we will only push for current tables
OLAPStatus PushHandler::process(
        SmartOLAPTable olap_table,
        const TPushReq& request,
        PushType push_type,
        vector<TTabletInfo>* tablet_info_vec) {
    OLAP_LOG_INFO("begin to push data. [table='%s' version=%ld]",
                   olap_table->full_name().c_str(), request.version);

    OLAPStatus res = OLAP_SUCCESS;
    _request = request;
    _olap_table_arr.clear();
    _olap_table_arr.push_back(olap_table);
    vector<TableVars> table_infoes(1);
    table_infoes[0].olap_table = olap_table;

    bool is_push_locked = false;
    bool is_new_tablet = false;
    bool is_new_tablet_effective = false;

    // 1. Get related tablets first if tablet in alter table status,
    TTabletId tablet_id;
    TSchemaHash schema_hash;
    AlterTabletType alter_table_type;
    SmartOLAPTable related_olap_table;
    _obtain_header_rdlock();
    bool is_schema_changing = olap_table->get_schema_change_request(
            &tablet_id, &schema_hash, NULL, &alter_table_type);
    _release_header_lock();

    if (is_schema_changing) {
        related_olap_table = OLAPEngine::get_instance()->get_table(tablet_id, schema_hash);
        if (NULL == related_olap_table.get()) {
            OLAP_LOG_WARNING("can't find olap table, clear invalid schema change info."
                             "[table=%ld schema_hash=%d]", tablet_id, schema_hash);
            _obtain_header_wrlock();
            olap_table->clear_schema_change_request();
            _release_header_lock();
            is_schema_changing = false;
        } else {
            // _olap_table_arr is used to obtain header lock,
            // to avoid deadlock, we must lock tablet header in time order.
            if (related_olap_table->creation_time() < olap_table->creation_time()) {
                _olap_table_arr.push_front(related_olap_table);
            } else {
                _olap_table_arr.push_back(related_olap_table);
            }
        }
    }

    // Obtain push lock to avoid simultaneously PUSH and
    // conflict with alter table operations.
    for (SmartOLAPTable table : _olap_table_arr) {
        table->obtain_push_lock();
        table->set_push_status(PUSH_RUNNING, _request.version);
    }
    is_push_locked = true;

    if (is_schema_changing) {
        _obtain_header_rdlock();
        is_schema_changing = olap_table->get_schema_change_request(
                &tablet_id, &schema_hash, NULL, &alter_table_type);
        _release_header_lock();

        if (!is_schema_changing) {
            OLAP_LOG_INFO("schema change info is cleared after base table get related tablet, "
                          "maybe new tablet reach at the same time and load firstly. ",
                          "[base_table='%s' table='%s' version=%ld]",
                          olap_table->full_name().c_str(),
                          related_olap_table->full_name().c_str(),
                          _request.version);
        } else if (related_olap_table->creation_time() > olap_table->creation_time()) {
            // If current table is old table, append it to table_infoes
            table_infoes.push_back(TableVars());
            TableVars& new_item = table_infoes.back();
            new_item.olap_table = related_olap_table;
        } else {
            // if current table is new table, clear schema change info
            res = _clear_alter_table_info(olap_table, related_olap_table);
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("fail to clear schema change info. [res=%d]", res);
                goto EXIT;
            }

            OLAP_LOG_INFO("data of new table is generated, stop convert from base table. "
                          "[base_table='%s' table='%s' version=%ld]",
                          related_olap_table->full_name().c_str(),
                          olap_table->full_name().c_str(),
                          _request.version);
            is_new_tablet_effective = true;
        }
    }

    // To keep logic of alter_table/rollup_table consistent
    if (table_infoes.size() == 1) {
        table_infoes.resize(2);
    }

    // 2. validate request: version and version_hash chek
    _obtain_header_rdlock();
    res = _validate_request(table_infoes[0].olap_table,
                            table_infoes[1].olap_table,
                            is_new_tablet_effective,
                            push_type);
    _release_header_lock();
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to validate request. [res=%d table='%s' version=%ld]",
                         res, olap_table->full_name().c_str(), _request.version);
        goto EXIT;
    }

    // 3. Remove reverted version including delta and cumulative,
    //    which will be deleted by background thread
    _obtain_header_wrlock();
    for (TableVars& table_var : table_infoes) {
        if (NULL == table_var.olap_table.get()) {
            continue;
        }

        res = _get_versions_reverted(table_var.olap_table,
                                     is_new_tablet,
                                     push_type,
                                     &(table_var.unused_versions));
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("failed to get reverted versions. "
                             "[res=%d table='%s' version=%ld]",
                             res, table_var.olap_table->full_name().c_str(), _request.version);
            goto EXIT;
        }

        if (table_var.unused_versions.size() != 0) {
            res = _update_header(table_var.olap_table,
                                 &(table_var.unused_versions),
                                 &(table_var.added_indices),
                                 &(table_var.unused_indices));
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("fail to update header for revert. "
                                 "[res=%d table='%s' version=%ld]",
                                 res, table_var.olap_table->full_name().c_str(), _request.version);
                goto EXIT;
            }

            _delete_old_indices(&(table_var.unused_indices));
        }

        // If there are more than one table, others is doing alter table
        is_new_tablet = true;
    }
    _release_header_lock();

    // 4. Save delete condition when push for delete
    if (push_type == PUSH_FOR_DELETE) {
        _obtain_header_wrlock();
        DeleteConditionHandler del_cond_handler;

        for (TableVars& table_var : table_infoes) {
            if (table_var.olap_table.get() == NULL) {
                continue;
            }

            res = del_cond_handler.store_cond(
                    table_var.olap_table, request.version, request.delete_conditions);
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("fail to store delete condition. [res=%d table='%s']",
                                 res, table_var.olap_table->full_name().c_str());
                goto EXIT;
            }

            res = table_var.olap_table->save_header();
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("fail to save header. [res=%d table='%s']",
                                 res, table_var.olap_table->full_name().c_str());
                goto EXIT;
            }
        }

        _release_header_lock();
    }

    // 5. Convert local data file into delta_file and build index,
    //    which may take a long time
    res = _convert(table_infoes[0].olap_table,
                   table_infoes[1].olap_table,
                   &(table_infoes[0].added_indices),
                   &(table_infoes[1].added_indices),
                   alter_table_type);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to convert data. [res=%d]", res);
        goto EXIT;
    }

    // Update table header: add new version and remove reverted version
    _obtain_header_wrlock();
    for (TableVars& table_var : table_infoes) {
        if (NULL == table_var.olap_table.get()) {
            continue;
        }

        res = _update_header(table_var.olap_table,
                             &(table_var.unused_versions),
                             &(table_var.added_indices),
                             &(table_var.unused_indices));
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("fail to update header of new delta."
                             "[res=%d table='%s' version=%ld]",
                             res, table_var.olap_table->full_name().c_str(), _request.version);
            goto EXIT;
        }
    }
    _release_header_lock();
    
    // 6. Delete unused versions which include delta and commulative,
    //    which, in fact, is added to list and deleted by background thread
    for (TableVars& table_var : table_infoes) {
        if (NULL == table_var.olap_table.get()) {
            continue;
        }

        _delete_old_indices(&(table_var.unused_indices));
    }

EXIT:
    _release_header_lock();

    // Get tablet infos for output
    if (res == OLAP_SUCCESS || res == OLAP_ERR_PUSH_VERSION_ALREADY_EXIST) {
        if (tablet_info_vec != NULL) {
            _get_tablet_infos(table_infoes, tablet_info_vec);
        }
        res = OLAP_SUCCESS;
    }

    // Clear added_indices when error happens
    for (TableVars& table_var : table_infoes) {
        if (table_var.olap_table.get() == NULL) {
            continue;
        }

        for (OLAPIndex* olap_index : table_var.added_indices) {
            olap_index->delete_all_files();
            SAFE_DELETE(olap_index);
        }
    }

    // Release push lock
    if (is_push_locked) {
        for (SmartOLAPTable table : _olap_table_arr) {
            table->release_push_lock();
            table->set_push_status(PUSH_WAITING, -1);
        }
    }
    _olap_table_arr.clear();

    OLAP_LOG_INFO("finish to process push. [res=%d]", res);

    return res;
}

void PushHandler::_get_tablet_infos(
        const vector<TableVars>& table_infoes,
        vector<TTabletInfo>* tablet_info_vec) {
    for (const TableVars& table_var : table_infoes) {
        if (table_var.olap_table.get() == NULL) {
            continue;
        }

        TTabletInfo tablet_info;
        tablet_info.tablet_id = table_var.olap_table->tablet_id();
        tablet_info.schema_hash = table_var.olap_table->schema_hash();
        OLAPEngine::get_instance()->report_tablet_info(&tablet_info);
        tablet_info_vec->push_back(tablet_info);
    }
}

OLAPStatus PushHandler::_convert(
        SmartOLAPTable curr_olap_table,
        SmartOLAPTable new_olap_table,
        Indices* curr_olap_indices,
        Indices* new_olap_indices,
        AlterTabletType alter_table_type) {
    OLAPStatus res = OLAP_SUCCESS;
    RowCursor row;
    BinaryFile raw_file;
    IBinaryReader* reader = NULL;
    IWriter* writer = NULL;
    OLAPIndex* delta_index = NULL;
    uint32_t  num_rows = 0;

    do {
        OLAP_LOG_DEBUG("start to convert delta file.");

        std::vector<FieldInfo> tablet_schema = curr_olap_table->tablet_schema();

        //curr_olap_table->set_tablet_schema();
        tablet_schema = curr_olap_table->tablet_schema();

        // 1. Init BinaryReader to read raw file if exist,
        //    in case of empty push and delete data, this will be skipped.
        if (_request.__isset.http_file_path) {
            // open raw file
            if (OLAP_SUCCESS != (res = raw_file.init(_request.http_file_path.c_str()))) {
                OLAP_LOG_WARNING("failed to read raw file. [res=%d file='%s']",
                                 res, _request.http_file_path.c_str());
                res = OLAP_ERR_INPUT_PARAMETER_ERROR;
                break;
            }

            // create BinaryReader
            bool need_decompress = false;
            if (_request.__isset.need_decompress && _request.need_decompress) {
                need_decompress = true;
            }
            if (NULL == (reader = IBinaryReader::create(need_decompress))) {
                OLAP_LOG_WARNING("fail to create reader. [table='%s' file='%s']",
                                 curr_olap_table->full_name().c_str(),
                                 _request.http_file_path.c_str());
                res = OLAP_ERR_MALLOC_ERROR;
                break;
            }

            // init BinaryReader
            if (OLAP_SUCCESS != (res = reader->init(curr_olap_table, &raw_file))) {
                OLAP_LOG_WARNING("fail to init reader. [res=%d table='%s' file='%s']",
                                 res,
                                 curr_olap_table->full_name().c_str(),
                                 _request.http_file_path.c_str());
                res = OLAP_ERR_PUSH_INIT_ERROR;
                break;
            }
        }

        // 2. New OLAPIndex of curr_olap_table for current push
        OLAP_LOG_DEBUG("init OLAPIndex.");

        if (NULL == (delta_index = new(std::nothrow) OLAPIndex(
                curr_olap_table.get(),
                Version(_request.version, _request.version),
                _request.version_hash,
                (_request.push_type == TPushType::LOAD_DELETE),
                0, 0))) {
            OLAP_LOG_WARNING("fail to malloc OLAPIndex. [table='%s' size=%ld]",
                             curr_olap_table->full_name().c_str(), sizeof(OLAPIndex));
            res = OLAP_ERR_MALLOC_ERROR;
            break;
        }
        curr_olap_indices->push_back(delta_index);

        // 3. New Writer to write data into OLAPIndex
        OLAP_LOG_DEBUG("init writer. [table='%s' block_row_size=%lu]",
                       curr_olap_table->full_name().c_str(),
                       curr_olap_table->num_rows_per_row_block());

        if (NULL == (writer = IWriter::create(curr_olap_table, delta_index, true))) {
            OLAP_LOG_WARNING("fail to create writer. [table='%s']",
                             curr_olap_table->full_name().c_str());
            res = OLAP_ERR_MALLOC_ERROR;
            break;
        } else if (OLAP_SUCCESS != (res = writer->init())) {
            OLAP_LOG_WARNING(
                    "fail to init writer. [res=%d table='%s' version=%u version_hash=%lu]",
                    res, curr_olap_table->full_name().c_str(),
                    _request.version, _request.version_hash);
            break;
        }

        MemPool* mem_pool = writer->mem_pool();
        // 4. Init RowCursor
        if (OLAP_SUCCESS != (res = row.init(curr_olap_table->tablet_schema()))) {
            OLAP_LOG_WARNING("fail to init rowcursor. [res=%d]", res);
            break;
        }

        // 5. Read data from raw file and write into OLAPIndex of curr_olap_table
        if (_request.__isset.http_file_path) {
            // Convert from raw to delta
            OLAP_LOG_DEBUG("start to convert row file to delta.");

            while (!reader->eof()) {
                if (OLAP_SUCCESS != (res = writer->attached_by(&row))) {
                    OLAP_LOG_WARNING(
                            "fail to attach row to writer. [res=%d table='%s' read_rows=%u]",
                            res, curr_olap_table->full_name().c_str(), num_rows);
                    break;
                }

                res = reader->next(&row, mem_pool);
                if (OLAP_SUCCESS != res) {
                    OLAP_LOG_WARNING("read next row failed. [res=%d read_rows=%u]",
                                     res, num_rows);
                    break;
                } else {
                    writer->next(row);
                    num_rows++;
                }
            }

            reader->finalize();

            if (false == reader->validate_checksum()) {
                OLAP_LOG_WARNING("pushed delta file has wrong checksum.");
                res = OLAP_ERR_PUSH_BUILD_DELTA_ERROR;
                break;
            }
        }

        if (OLAP_SUCCESS != (res = writer->finalize())) {
            OLAP_LOG_WARNING("fail to finalize writer. [res=%d]", res);
            break;
        }

        OLAP_LOG_DEBUG("load the index.");

        if (OLAP_SUCCESS != (res = delta_index->load())) {
            OLAP_LOG_WARNING("fail to load index. [res=%d table='%s' version=%ld]",
                             res, curr_olap_table->full_name().c_str(), _request.version);
            break;
        }
        _write_bytes += delta_index->data_size();
        _write_rows += delta_index->num_rows();

        // 7. Convert data for schema change tables
        OLAP_LOG_TRACE("load to related tables of schema_change if possible. ");
        if (NULL != new_olap_table.get()) {
            SchemaChangeHandler schema_change;
            res = schema_change.schema_version_convert(
                    curr_olap_table,
                    new_olap_table,
                    curr_olap_indices,
                    new_olap_indices);
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_WARNING("failed to change schema version for delta."
                                 "[res=%d new_table='%s']",
                                 res, new_olap_table->full_name().c_str());
            }

        }
    } while (0);

    SAFE_DELETE(reader);
    SAFE_DELETE(writer);
    OLAP_LOG_NOTICE_PUSH("processed_rows", "%d", num_rows);
    OLAP_LOG_TRACE("convert delta file end. [table='%s' res=%d]",
                   curr_olap_table->full_name().c_str(), res);

    return res;
}

OLAPStatus PushHandler::_validate_request(
        SmartOLAPTable olap_table_for_raw,
        SmartOLAPTable olap_table_for_schema_change,
        bool is_new_tablet_effective,
        PushType push_type) {
    const FileVersionMessage* latest_delta = olap_table_for_raw->lastest_delta();

    if (NULL == latest_delta) {
        const FileVersionMessage* latest_version = olap_table_for_raw->latest_version();

        // PUSH the first version when the version is 0, or
        // tablet is in alter table status.
        if (NULL == latest_version
                && (0 == _request.version || NULL != olap_table_for_schema_change.get())) {
            return OLAP_SUCCESS;
        } else if (NULL != latest_version
                && (latest_version->end_version() + 1 == _request.version)) {
            return OLAP_SUCCESS;
        }

        OLAP_LOG_WARNING("no last pushed delta, the comming version should be 0. [table='%s']",
                         olap_table_for_raw->full_name().c_str());
        return OLAP_ERR_PUSH_VERSION_INCORRECT;
    }

    if (is_new_tablet_effective) {
        OLAP_LOG_INFO("maybe a alter tablet has already created from base tablet. "
                       "[table='%s' version=%d]",
                       olap_table_for_raw->full_name().c_str(),
                       _request.version);
        if (push_type == PUSH_FOR_DELETE
                && _request.version == latest_delta->start_version()
                && _request.version_hash == latest_delta->version_hash()) {
            OLAP_LOG_INFO("base tablet has already convert delete version for new tablet. "
                          "[version=%ld version_hash=%lu]",
                          _request.version, _request.version_hash);
            return OLAP_ERR_PUSH_VERSION_ALREADY_EXIST;
        }
    } else {
        // Never allow two push has same version and version hash,
        // but same verson and different version hash is allowed.
        if (_request.version < latest_delta->start_version()
                || _request.version > latest_delta->start_version() + 1) {
            OLAP_LOG_WARNING(
                    "try to push a delta with incorrect version. "
                    "[new_version=%ld latest_version=%u "
                    "new_version_hash=%ld latest_version_hash=%lu]",
                    _request.version, latest_delta->start_version(),
                    _request.version_hash, latest_delta->version_hash());
            return OLAP_ERR_PUSH_VERSION_INCORRECT;
        } else if (_request.version == latest_delta->start_version()
                && _request.version_hash == latest_delta->version_hash()) {
            OLAP_LOG_WARNING(
                    "try to push a already exist delta. "
                    "[new_version=%ld latest_version=%u "
                    "new_version_hash=%ld latest_version_hash=%lu]",
                    _request.version, latest_delta->start_version(),
                    _request.version_hash, latest_delta->version_hash());
            return OLAP_ERR_PUSH_VERSION_ALREADY_EXIST;
        }
    }

    return OLAP_SUCCESS;
}


// The latest version can be reverted for following scene:
// user submit a push job and cancel it soon, but some 
// tablets already push success.
OLAPStatus PushHandler::_get_versions_reverted(
        SmartOLAPTable olap_table,
        bool is_new_tablet,
        PushType push_type,
        Versions* unused_versions) {
    const FileVersionMessage* latest_delta = olap_table->lastest_delta();

    if (NULL == latest_delta) {
        const FileVersionMessage* latest_version = olap_table->latest_version();

        // PUSH the first version, and the version is 0
        if ((NULL == latest_version
                && (0 == _request.version || is_new_tablet))) {
            return OLAP_SUCCESS;
        } else if (NULL != latest_version
                && latest_version->end_version() + 1 == _request.version) {
            return OLAP_SUCCESS;
        }

        OLAP_LOG_WARNING("no last pushed delta, the comming version should be 0. [table='%s']",
                         olap_table->full_name().c_str());
        return OLAP_ERR_PUSH_VERSION_INCORRECT;
    }

    OLAP_LOG_DEBUG("latest deltas was founded. [table='%s' version=%d-%d]",
                   olap_table->full_name().c_str(),
                   latest_delta->start_version(), latest_delta->end_version());

    // Remove the cumulative delta that end_version == request.version()
    if (_request.version == latest_delta->start_version()) {
        Versions all_versions;
        olap_table->list_versions(&all_versions);

        for (Versions::const_iterator v = all_versions.begin(); v != all_versions.end(); ++v) {
            if (v->second == _request.version) {
                unused_versions->push_back(*v);
                OLAP_LOG_DEBUG("Add unused version. [table='%s' version=%d-%d]",
                               olap_table->full_name().c_str(), v->first, v->second);
            }
        }

        // Remove delete condition if current type is PUSH_FOR_DELETE,
        // this occurs when user cancel delete_data soon after submit it.
        if (push_type != PUSH_FOR_DELETE) {
            DeleteConditionHandler del_cond_handler;
            del_cond_handler.delete_cond(olap_table, _request.version, false);
        }
    }

    return OLAP_SUCCESS;
}

OLAPStatus PushHandler::_update_header(
        SmartOLAPTable olap_table,
        Versions* unused_versions,
        Indices* new_indices,
        Indices* unused_indices) {
    OLAPStatus res = OLAP_SUCCESS;

    res = olap_table->replace_data_sources(
            unused_versions,
            new_indices,
            unused_indices);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_FATAL("fail to replace data sources. [res=%d table='%s']",
                       res, olap_table->full_name().c_str());
        return res;
    }

    // Avoid double update
    new_indices->clear();
    unused_versions->clear();

    // Save header fail will not impact service for memory state
    // has already changed, but some data may lost when OLAPEngine restart;
    // Note we don't return fail here.
    res = olap_table->save_header();
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_FATAL("fail to save header. [res=%d table='%s']",
                       res, olap_table->full_name().c_str());
    }

    return res;
}

void PushHandler::_delete_old_indices(Indices* unused_indices) {
    if (!unused_indices->empty()) {
        OLAPUnusedIndex* unused_index = OLAPUnusedIndex::get_instance();

        for (Indices::iterator it = unused_indices->begin();
                it != unused_indices->end(); ++it) {
            unused_index->add_unused_index(*it);
        }
    }
}

OLAPStatus PushHandler::_clear_alter_table_info(
        SmartOLAPTable tablet,
        SmartOLAPTable related_tablet) {
    OLAPStatus res = OLAP_SUCCESS;
    _obtain_header_wrlock();

    do {
        res = SchemaChangeHandler::clear_schema_change_single_info(
                tablet, NULL, false, false);
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_FATAL("fail to clear schema change info of new table. [res=%d table='%s']",
                           res, tablet->full_name().c_str());
            break;
        }
        
        res = tablet->save_header();
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_FATAL("fail to save header. [res=%d table='%s']",
                           res, tablet->full_name().c_str());
            break;
        }

        TTabletId tablet_id;
        TSchemaHash schema_hash;
        bool is_sc = related_tablet->get_schema_change_request(
                &tablet_id, &schema_hash, NULL, NULL);
        if (is_sc && tablet_id == tablet->tablet_id() && schema_hash == tablet->schema_hash()) {
            res = SchemaChangeHandler::clear_schema_change_single_info(
                    related_tablet, NULL, false, false);
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_FATAL("fail to clear schema change info of old table. [res=%d table='%s']",
                               res, related_tablet->full_name().c_str());
                break;
            }
            
            res = related_tablet->save_header();
            if (res != OLAP_SUCCESS) {
                OLAP_LOG_FATAL("fail to save header. [res=%d table='%s']",
                               res, related_tablet->full_name().c_str());
                break;
            }
        }
    } while (0);

    _release_header_lock();
    return res;
}

OLAPStatus BinaryFile::init(const char* path) {
    // open file
    if (OLAP_SUCCESS != open(path, "rb")) {
        OLAP_LOG_WARNING("fail to open file. [file='%s']", path);
        return OLAP_ERR_IO_ERROR;
    }

    // load header
    if (OLAP_SUCCESS != _header.unserialize(this)) {
        OLAP_LOG_WARNING("fail to read file header. [file='%s']", path);
        close();
        return OLAP_ERR_PUSH_INIT_ERROR;
    }

    return OLAP_SUCCESS;
}

IBinaryReader* IBinaryReader::create(bool need_decompress) {
    IBinaryReader* reader = NULL;
    if (need_decompress) {
        reader = new(std::nothrow) LzoBinaryReader();
    } else {
        reader = new(std::nothrow) BinaryReader();
    }
    return reader;
}

BinaryReader::BinaryReader()
    : IBinaryReader(),
      _row_buf(NULL),
      _row_buf_size(0) {
}

OLAPStatus BinaryReader::init(
        SmartOLAPTable table,
        BinaryFile* file) {
    OLAPStatus res = OLAP_SUCCESS;

    do {
        _file = file;
        _content_len = _file->file_length() - _file->header_size();
        _row_buf_size = table->get_row_size();

        if (NULL == (_row_buf = new(std::nothrow) char[_row_buf_size])) {
            OLAP_LOG_WARNING("fail to malloc one row buf. [size=%zu]", _row_buf_size);
            res = OLAP_ERR_MALLOC_ERROR;
            break;
        }

        if (-1 == _file->seek(_file->header_size(), SEEK_SET)) {
            OLAP_LOG_WARNING("skip header, seek fail.");
            res = OLAP_ERR_IO_ERROR;
            break;
        }

        _table = table;
        _ready = true;
    } while (0);

    if (res != OLAP_SUCCESS) {
        SAFE_DELETE_ARRAY(_row_buf);
    }
    return res;
}

OLAPStatus BinaryReader::finalize() {
    _ready = false;
    SAFE_DELETE_ARRAY(_row_buf);
    return OLAP_SUCCESS;
}

OLAPStatus BinaryReader::next(RowCursor* row, MemPool* mem_pool) {
    OLAPStatus res = OLAP_SUCCESS;

    if (!_ready || NULL == row) {
        // Here i assume _ready means all states were set up correctly
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    const vector<FieldInfo>& schema = _table->tablet_schema();
    size_t offset = 0;
    size_t field_size = 0;
    size_t num_null_bytes = (_table->num_null_fields() + 7) / 8;

    if (OLAP_SUCCESS != (res = _file->read(_row_buf + offset, num_null_bytes))) {
        OLAP_LOG_WARNING("read file for one row fail. [res=%d]", res);
        return res;
    }

    size_t p  = 0;
    for (size_t i = 0; i < schema.size(); ++i) {
        row->set_not_null(i);
        if (schema[i].is_allow_null) {
            bool is_null = false;
            is_null = (_row_buf[p/8] >> ((num_null_bytes * 8 - p - 1) % 8)) & 1;
            if (is_null) {
                row->set_null(i);
            }
            p++;
        }
    }
    offset += num_null_bytes;

    for (uint32_t i = 0; i < schema.size(); i++) {
        if (row->is_null(i)) {
            continue;
        }
        if (schema[i].type == OLAP_FIELD_TYPE_VARCHAR || schema[i].type == OLAP_FIELD_TYPE_HLL) {
            // Read varchar length buffer first
            if (OLAP_SUCCESS != (res = _file->read(_row_buf + offset,
                        sizeof(StringLengthType)))) {
                OLAP_LOG_WARNING("read file for one row fail. [res=%d]", res);
                return res;
            }

            // Get varchar field size
            field_size = *reinterpret_cast<StringLengthType*>(_row_buf + offset);
            offset += sizeof(StringLengthType);
            if (field_size > schema[i].length - sizeof(StringLengthType)) {
                OLAP_LOG_WARNING("invalid data length for VARCHAR! [max_len=%d real_len=%d]",
                                 schema[i].length - sizeof(StringLengthType),
                                 field_size);
                return OLAP_ERR_PUSH_INPUT_DATA_ERROR;
            }
        } else {
            field_size = schema[i].length;
        }

        // Read field content according to field size
        if (OLAP_SUCCESS != (res = _file->read(_row_buf + offset, field_size))) {
            OLAP_LOG_WARNING("read file for one row fail. [res=%d]", res);
            return res;
        }

        if (schema[i].type == OLAP_FIELD_TYPE_CHAR
                || schema[i].type == OLAP_FIELD_TYPE_VARCHAR
                || schema[i].type == OLAP_FIELD_TYPE_HLL) {
            StringSlice slice(_row_buf + offset, field_size);
            row->set_field_content(i, reinterpret_cast<char*>(&slice), mem_pool);
        } else {
            row->set_field_content(i, _row_buf + offset, mem_pool);
        }
        offset += field_size;
    }
    _curr += offset;

    // Calculate checksum for validate when push finished.
    _adler_checksum = olap_adler32(_adler_checksum, _row_buf, offset);
    return res;
}

LzoBinaryReader::LzoBinaryReader()
    : IBinaryReader(),
      _row_buf(NULL),
      _row_compressed_buf(NULL),
      _row_info_buf(NULL),
      _max_row_num(0),
      _max_row_buf_size(0),
      _max_compressed_buf_size(0),
      _row_num(0),
      _next_row_start(0) {
}

OLAPStatus LzoBinaryReader::init(
        SmartOLAPTable table,
        BinaryFile* file) {
    OLAPStatus res = OLAP_SUCCESS;

    do {
        _file = file;
        _content_len = _file->file_length() - _file->header_size();

        size_t row_info_buf_size = sizeof(RowNumType) + sizeof(CompressedSizeType);
        if (NULL == (_row_info_buf = new(std::nothrow) char[row_info_buf_size])) {
            OLAP_LOG_WARNING("fail to malloc rows info buf. [size=%zu]", row_info_buf_size);
            res = OLAP_ERR_MALLOC_ERROR;
            break;
        }

        if (-1 == _file->seek(_file->header_size(), SEEK_SET)) {
            OLAP_LOG_WARNING("skip header, seek fail.");
            res = OLAP_ERR_IO_ERROR;
            break;
        }

        _table = table;
        _ready = true;
    } while (0);

    if (res != OLAP_SUCCESS) {
        SAFE_DELETE_ARRAY(_row_info_buf);
    }
    return res;
}

OLAPStatus LzoBinaryReader::finalize() {
    _ready = false;
    SAFE_DELETE_ARRAY(_row_buf);
    SAFE_DELETE_ARRAY(_row_compressed_buf);
    SAFE_DELETE_ARRAY(_row_info_buf);
    return OLAP_SUCCESS;
}

OLAPStatus LzoBinaryReader::next(RowCursor* row, MemPool* mem_pool) {
    OLAPStatus res = OLAP_SUCCESS;

    if (!_ready || NULL == row) {
        // Here i assume _ready means all states were set up correctly
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    if (_row_num == 0) {
        // read next block
        if (OLAP_SUCCESS != (res = _next_block())) {
            return res;
        }
    }

    const vector<FieldInfo>& schema = _table->tablet_schema();
    size_t offset = 0;
    size_t field_size = 0;
    size_t num_null_bytes = (_table->num_null_fields() + 7) / 8;

    size_t p = 0;
    for (size_t i = 0; i < schema.size(); ++i) {
        row->set_not_null(i);
        if (schema[i].is_allow_null) {
            bool is_null = false;
            is_null = (_row_buf[_next_row_start + p/8] >> ((num_null_bytes * 8 - p - 1) % 8)) & 1;
            if (is_null) {
                row->set_null(i);
            }
            p++;
        }
    }
    offset += num_null_bytes;

    for (uint32_t i = 0; i < schema.size(); i++) {
        if (row->is_null(i)) {
            continue;
        }

        if (schema[i].type == OLAP_FIELD_TYPE_VARCHAR || schema[i].type == OLAP_FIELD_TYPE_HLL) {
            // Get varchar field size
            field_size = *reinterpret_cast<StringLengthType*>(_row_buf + _next_row_start + offset);
            offset += sizeof(StringLengthType);

            if (field_size > schema[i].length - sizeof(StringLengthType)) {
                OLAP_LOG_WARNING("invalid data length for VARCHAR! [max_len=%d real_len=%d]",
                                 schema[i].length - sizeof(StringLengthType),
                                 field_size);
                return OLAP_ERR_PUSH_INPUT_DATA_ERROR;
            }
        } else {
            field_size = schema[i].length;
        }

        if (schema[i].type == OLAP_FIELD_TYPE_CHAR
                || schema[i].type == OLAP_FIELD_TYPE_VARCHAR
                || schema[i].type == OLAP_FIELD_TYPE_HLL) {
            StringSlice slice(_row_buf + _next_row_start + offset, field_size);
            row->set_field_content(i, reinterpret_cast<char*>(&slice), mem_pool);
        } else {
            row->set_field_content(i, _row_buf + _next_row_start + offset, mem_pool);
        }

        offset += field_size;
    }

    // Calculate checksum for validate when push finished.
    _adler_checksum = olap_adler32(_adler_checksum, _row_buf + _next_row_start, offset);

    _next_row_start += offset;
    --_row_num;
    return res;
}

OLAPStatus LzoBinaryReader::_next_block() {
    OLAPStatus res = OLAP_SUCCESS;

    // Get row num and compressed data size
    size_t row_info_buf_size = sizeof(RowNumType) + sizeof(CompressedSizeType);
    if (OLAP_SUCCESS != (res = _file->read(_row_info_buf, row_info_buf_size))) {
        OLAP_LOG_WARNING("read rows info fail. [res=%d]", res);
        return res;
    }

    RowNumType* rows_num_ptr = reinterpret_cast<RowNumType*>(_row_info_buf);
    _row_num = *rows_num_ptr;
    CompressedSizeType* compressed_size_ptr = reinterpret_cast<CompressedSizeType*>(
        _row_info_buf + sizeof(RowNumType));
    CompressedSizeType compressed_size = *compressed_size_ptr;

    if (_row_num > _max_row_num) {
        // renew rows buf
        SAFE_DELETE_ARRAY(_row_buf);

        _max_row_num = _row_num;
        _max_row_buf_size = _max_row_num * _table->get_row_size();
        if (NULL == (_row_buf = new(std::nothrow) char[_max_row_buf_size])) {
            OLAP_LOG_WARNING("fail to malloc rows buf. [size=%zu]", _max_row_buf_size);
            res = OLAP_ERR_MALLOC_ERROR;
            return res;
        }
    }

    if (compressed_size > _max_compressed_buf_size) {
        // renew rows compressed buf
        SAFE_DELETE_ARRAY(_row_compressed_buf);

        _max_compressed_buf_size = compressed_size;
        if (NULL == (_row_compressed_buf = new(std::nothrow) char[_max_compressed_buf_size])) {
            OLAP_LOG_WARNING("fail to malloc rows compressed buf. [size=%zu]", _max_compressed_buf_size);
            res = OLAP_ERR_MALLOC_ERROR;
            return res;
        }
    }

    if (OLAP_SUCCESS != (res = _file->read(_row_compressed_buf, compressed_size))) {
        OLAP_LOG_WARNING("read compressed rows fail. [res=%d]", res);
        return res;
    }

    // python lzo use lzo1x to compress
    // and add 5 bytes header (\xf0 + 4 bytes(uncompress data size))
    size_t written_len = 0;
    size_t block_header_size = 5;
    if (OLAP_SUCCESS != (res = olap_decompress(_row_compressed_buf + block_header_size, 
                                               compressed_size - block_header_size,
                                               _row_buf, 
                                               _max_row_buf_size, 
                                               &written_len, 
                                               OLAP_COMP_TRANSPORT))) {
        OLAP_LOG_WARNING("olap decompress fail. [res=%d]", res);
        return res;
    }

    _curr += row_info_buf_size + compressed_size;
    _next_row_start = 0;
    return res;
}

string PushHandler::_debug_version_list(const Versions& versions) const {
    std::ostringstream txt;
    txt << "Versions: ";

    for (Versions::const_iterator it = versions.begin(); it != versions.end(); ++it) {
        txt << "[" << it->first << "~" << it->second << "],";
    }

    return txt.str();
}

}  // namespace palo
