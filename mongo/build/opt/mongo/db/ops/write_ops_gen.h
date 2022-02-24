/**
 * WARNING: This is a generated file. Do not modify.
 *
 * Source: buildscripts/idl/idlc.py --include src --base_dir build/opt --header build/opt/mongo/db/ops/write_ops_gen.h --output build/opt/mongo/db/ops/write_ops_gen.cpp src/mongo/db/ops/write_ops.idl
 */

#pragma once

#include <algorithm>
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/logical_session_id_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace write_ops {

/**
 * Contains basic information included by all write commands
 */
class WriteCommandBase {
public:
    static constexpr auto kBypassDocumentValidationFieldName = "bypassDocumentValidation"_sd;
    static constexpr auto kOrderedFieldName = "ordered"_sd;
    static constexpr auto kStmtIdsFieldName = "stmtIds"_sd;

    WriteCommandBase();

    static WriteCommandBase parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;

    /**
     * Enables the operation to bypass document validation. This lets you write documents that do not meet the validation requirements.
     */
    bool getBypassDocumentValidation() const { return _bypassDocumentValidation; }
    void setBypassDocumentValidation(bool value) & { _bypassDocumentValidation = std::move(value);  }

    /**
     * If true, then when an write statement fails, the command returns without executing the remaining statements. If false, then statements are allowed to be executed in parallel and if a statement fails, continue with the remaining statements, if any.
     */
    bool getOrdered() const { return _ordered; }
    void setOrdered(bool value) & { _ordered = std::move(value);  }

    /**
     * An array of statement numbers relative to the transaction. If this field is set, its size must be exactly the same as the number of entries in the corresponding insert/update/delete request. If it is not set, the statement ids of the contained operation will be implicitly generated based on their offset, starting from 0.
     */
    const boost::optional<std::vector<std::int32_t>>& getStmtIds() const& { return _stmtIds; }
    void getStmtIds() && = delete;
    void setStmtIds(boost::optional<std::vector<std::int32_t>> value) & { _stmtIds = std::move(value);  }

protected:
    void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);

private:
    bool _bypassDocumentValidation{false};
    bool _ordered{true};
    boost::optional<std::vector<std::int32_t>> _stmtIds;
};

/**
 * Parser for the entries in the 'updates' array of an update command.
 */
class UpdateOpEntry {
public:
    static constexpr auto kArrayFiltersFieldName = "arrayFilters"_sd;
    static constexpr auto kCollationFieldName = "collation"_sd;
    static constexpr auto kMultiFieldName = "multi"_sd;
    static constexpr auto kQFieldName = "q"_sd;
    static constexpr auto kUFieldName = "u"_sd;
    static constexpr auto kUpsertFieldName = "upsert"_sd;

    UpdateOpEntry();

    static UpdateOpEntry parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;

    /**
     * The query that matches documents to update. Uses the same query selectors as used in the 'find' operation.
     */
    const mongo::BSONObj& getQ() const { return _q; }
    void setQ(mongo::BSONObj value) & { _q = std::move(value); _hasQ = true; }

    /**
     * Set of modifications to apply.
     */
    const mongo::BSONObj& getU() const { return _u; }
    void setU(mongo::BSONObj value) & { _u = std::move(value); _hasU = true; }

    /**
     * Specifies which array elements an update modifier should apply to.
     */
    const boost::optional<std::vector<mongo::BSONObj>>& getArrayFilters() const& { return _arrayFilters; }
    void getArrayFilters() && = delete;
    void setArrayFilters(boost::optional<std::vector<mongo::BSONObj>> value) & { _arrayFilters = std::move(value);  }

    /**
     * If true, updates all documents that meet the query criteria. If false, limits the update to one document which meets the query criteria.
     */
    bool getMulti() const { return _multi; }
    void setMulti(bool value) & { _multi = std::move(value);  }

    /**
     * If true, perform an insert if no documents match the query. If both upsert and multi are true and no documents match the query, the update operation inserts only a single document.
     */
    bool getUpsert() const { return _upsert; }
    void setUpsert(bool value) & { _upsert = std::move(value);  }

    /**
     * Specifies the collation to use for the operation.
     */
    const boost::optional<mongo::BSONObj>& getCollation() const& { return _collation; }
    void getCollation() && = delete;
    void setCollation(boost::optional<mongo::BSONObj> value) & { _collation = std::move(value);  }

protected:
    void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);

private:
    mongo::BSONObj _q;
    mongo::BSONObj _u;
    boost::optional<std::vector<mongo::BSONObj>> _arrayFilters;
    bool _multi{false};
    bool _upsert{false};
    boost::optional<mongo::BSONObj> _collation;
    bool _hasQ : 1;
    bool _hasU : 1;
};

/**
 * Parser for the entries in the 'deletes' array of a delete command.
 */
class DeleteOpEntry {
public:
    static constexpr auto kCollationFieldName = "collation"_sd;
    static constexpr auto kMultiFieldName = "limit"_sd;
    static constexpr auto kQFieldName = "q"_sd;

    DeleteOpEntry();

    static DeleteOpEntry parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;

    /**
     * The query that matches documents to delete. Uses the same query selectors as used in the 'find' operation.
     */
    const mongo::BSONObj& getQ() const { return _q; }
    void setQ(mongo::BSONObj value) & { _q = std::move(value); _hasQ = true; }

    /**
     * The number of matching documents to delete. Value of 0 deletes all matching documents and 1 deletes a single document.
     */
    bool getMulti() const { return _multi; }
    void setMulti(bool value) & { _multi = std::move(value); _hasMulti = true; }

    /**
     * Specifies the collation to use for the operation.
     */
    const boost::optional<mongo::BSONObj>& getCollation() const& { return _collation; }
    void getCollation() && = delete;
    void setCollation(boost::optional<mongo::BSONObj> value) & { _collation = std::move(value);  }

protected:
    void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);

private:
    mongo::BSONObj _q;
    bool _multi;
    boost::optional<mongo::BSONObj> _collation;
    bool _hasQ : 1;
    bool _hasMulti : 1;
};

/**
 * Parser for the 'insert' command.
 */
class Insert {
public:
    static constexpr auto kBypassDocumentValidationFieldName = "bypassDocumentValidation"_sd;
    static constexpr auto kDbNameFieldName = "$db"_sd;
    static constexpr auto kDocumentsFieldName = "documents"_sd;
    static constexpr auto kOrderedFieldName = "ordered"_sd;
    static constexpr auto kStmtIdsFieldName = "stmtIds"_sd;
    static constexpr auto kWriteCommandBaseFieldName = "WriteCommandBase"_sd;
    static constexpr auto kCommandName = "insert"_sd;

    explicit Insert(const NamespaceString nss);

    static Insert parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    static Insert parse(const IDLParserErrorContext& ctxt, const OpMsgRequest& request);
    void serialize(const BSONObj& commandPassthroughFields, BSONObjBuilder* builder) const;
    OpMsgRequest serialize(const BSONObj& commandPassthroughFields) const;
    BSONObj toBSON(const BSONObj& commandPassthroughFields) const;

    const NamespaceString& getNamespace() const { return _nss; }

    /**
     * Contains basic information included by all write commands
     */
    const WriteCommandBase& getWriteCommandBase() const { return _writeCommandBase; }
    WriteCommandBase& getWriteCommandBase() { return _writeCommandBase; }
    void setWriteCommandBase(WriteCommandBase value) & { _writeCommandBase = std::move(value);  }

    /**
     * An array of one or more documents to insert.
     */
    const std::vector<mongo::BSONObj>& getDocuments() const& { return _documents; }
    void getDocuments() && = delete;
    void setDocuments(std::vector<mongo::BSONObj> value) & { _documents = std::move(value); _hasDocuments = true; }

    const StringData getDbName() const& { return _dbName; }
    void getDbName() && = delete;
    void setDbName(StringData value) & { _dbName = value.toString(); _hasDbName = true; }

protected:
    void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    void parseProtected(const IDLParserErrorContext& ctxt, const OpMsgRequest& request);

private:
    static const std::vector<StringData> _knownFields;


    NamespaceString _nss;

    WriteCommandBase _writeCommandBase;
    std::vector<mongo::BSONObj> _documents;
    std::string _dbName;
    bool _hasDocuments : 1;
    bool _hasDbName : 1;
};

/**
 * Parser for the 'update' command.
 */
class Update {
public:
    static constexpr auto kBypassDocumentValidationFieldName = "bypassDocumentValidation"_sd;
    static constexpr auto kDbNameFieldName = "$db"_sd;
    static constexpr auto kOrderedFieldName = "ordered"_sd;
    static constexpr auto kStmtIdsFieldName = "stmtIds"_sd;
    static constexpr auto kUpdatesFieldName = "updates"_sd;
    static constexpr auto kWriteCommandBaseFieldName = "WriteCommandBase"_sd;
    static constexpr auto kCommandName = "update"_sd;

    explicit Update(const NamespaceString nss);

    static Update parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    static Update parse(const IDLParserErrorContext& ctxt, const OpMsgRequest& request);
    void serialize(const BSONObj& commandPassthroughFields, BSONObjBuilder* builder) const;
    OpMsgRequest serialize(const BSONObj& commandPassthroughFields) const;
    BSONObj toBSON(const BSONObj& commandPassthroughFields) const;

    const NamespaceString& getNamespace() const { return _nss; }

    /**
     * Contains basic information included by all write commands
     */
    const WriteCommandBase& getWriteCommandBase() const { return _writeCommandBase; }
    WriteCommandBase& getWriteCommandBase() { return _writeCommandBase; }
    void setWriteCommandBase(WriteCommandBase value) & { _writeCommandBase = std::move(value);  }

    /**
     * An array of one or more update statements to perform.
     */
    const std::vector<UpdateOpEntry>& getUpdates() const& { return _updates; }
    void getUpdates() && = delete;
    void setUpdates(std::vector<UpdateOpEntry> value) & { _updates = std::move(value); _hasUpdates = true; }

    const StringData getDbName() const& { return _dbName; }
    void getDbName() && = delete;
    void setDbName(StringData value) & { _dbName = value.toString(); _hasDbName = true; }

protected:
    void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    void parseProtected(const IDLParserErrorContext& ctxt, const OpMsgRequest& request);

private:
    static const std::vector<StringData> _knownFields;


    NamespaceString _nss;

    WriteCommandBase _writeCommandBase;
    std::vector<UpdateOpEntry> _updates;
    std::string _dbName;
    bool _hasUpdates : 1;
    bool _hasDbName : 1;
};

/**
 * Parser for the 'delete' command.
 */
class Delete {
public:
    static constexpr auto kBypassDocumentValidationFieldName = "bypassDocumentValidation"_sd;
    static constexpr auto kDbNameFieldName = "$db"_sd;
    static constexpr auto kDeletesFieldName = "deletes"_sd;
    static constexpr auto kOrderedFieldName = "ordered"_sd;
    static constexpr auto kStmtIdsFieldName = "stmtIds"_sd;
    static constexpr auto kWriteCommandBaseFieldName = "WriteCommandBase"_sd;
    static constexpr auto kCommandName = "delete"_sd;

    explicit Delete(const NamespaceString nss);

    static Delete parse(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    static Delete parse(const IDLParserErrorContext& ctxt, const OpMsgRequest& request);
    void serialize(const BSONObj& commandPassthroughFields, BSONObjBuilder* builder) const;
    OpMsgRequest serialize(const BSONObj& commandPassthroughFields) const;
    BSONObj toBSON(const BSONObj& commandPassthroughFields) const;

    const NamespaceString& getNamespace() const { return _nss; }

    /**
     * Contains basic information included by all write commands
     */
    const WriteCommandBase& getWriteCommandBase() const { return _writeCommandBase; }
    WriteCommandBase& getWriteCommandBase() { return _writeCommandBase; }
    void setWriteCommandBase(WriteCommandBase value) & { _writeCommandBase = std::move(value);  }

    /**
     * An array of one or more delete statements to perform.
     */
    const std::vector<DeleteOpEntry>& getDeletes() const& { return _deletes; }
    void getDeletes() && = delete;
    void setDeletes(std::vector<DeleteOpEntry> value) & { _deletes = std::move(value); _hasDeletes = true; }

    const StringData getDbName() const& { return _dbName; }
    void getDbName() && = delete;
    void setDbName(StringData value) & { _dbName = value.toString(); _hasDbName = true; }

protected:
    void parseProtected(const IDLParserErrorContext& ctxt, const BSONObj& bsonObject);
    void parseProtected(const IDLParserErrorContext& ctxt, const OpMsgRequest& request);

private:
    static const std::vector<StringData> _knownFields;


    NamespaceString _nss;

    WriteCommandBase _writeCommandBase;
    std::vector<DeleteOpEntry> _deletes;
    std::string _dbName;
    bool _hasDeletes : 1;
    bool _hasDbName : 1;
};

}  // namespace write_ops
}  // namespace mongo
