#include "ofexcel/local_validator.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/cte_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/set_operation_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/list.hpp"

#include <algorithm>
#include <cctype>

namespace ofexcel {

namespace {

using duckdb::ExpressionClass;
using duckdb::ParsedExpression;
using duckdb::ParsedExpressionIterator;
using duckdb::QueryNode;
using duckdb::QueryNodeType;
using duckdb::TableReferenceType;
using duckdb::TableRef;

//! V6.
const char *const ALLOWED_TABLE_FUNCTIONS[] = {"unnest", "range", "generate_series"};
//! V7, exact names.
const char *const FORBIDDEN_FUNCTIONS[] = {"getenv", "current_setting", "read_text", "read_blob"};
//! V7, prefixes.
const char *const FORBIDDEN_PREFIXES[] = {"ofexcel_", "fusion_scanner_", "oracle_fusion_"};

std::string Lower(std::string text) {
	for (auto &c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}

std::string Name(const duckdb::Identifier &identifier) {
	return identifier.GetIdentifierName();
}

bool Contains(const std::vector<std::string> &names, const std::string &name) {
	return std::find(names.begin(), names.end(), Lower(name)) != names.end();
}

struct Violation {
	std::string rule;
	std::string fragment;
	std::string message;
};

[[noreturn]] void Violate(const char *rule, const std::string &fragment, const std::string &message) {
	throw Violation {rule, fragment, message};
}

//! Walks one statement. The CTE names in scope travel down with each node,
//! because a CTE is visible in its own query and below it, not beside it.
class Validator {
public:
	explicit Validator(const LocalSqlScope &scope) : scope(scope) {
	}

	void Statement(duckdb::SQLStatement &statement) {
		if (statement.type != duckdb::StatementType::SELECT_STATEMENT) {
			Violate("V2", duckdb::StatementTypeToString(statement.type),
			        "разрешена только инструкция SELECT (в том числе WITH, UNION, VALUES)");
		}
		Node(*statement.Cast<duckdb::SelectStatement>().node, {});
	}

private:
	void Node(QueryNode &node, std::vector<std::string> ctes) {
		for (auto &entry : node.cte_map.map) {
			ctes.push_back(Lower(Name(entry.first)));
		}
		// V9: a CTE's own query follows the same rules, and may name any CTE of
		// this WITH (a recursive one names itself).
		for (auto &entry : node.cte_map.map) {
			if (entry.second->query_node) {
				Node(*entry.second->query_node, ctes);
			}
		}
		switch (node.type) {
		case QueryNodeType::SELECT_NODE: {
			auto &select = node.Cast<duckdb::SelectNode>();
			for (auto &expression : select.select_list) {
				Expression(expression, ctes);
			}
			for (auto &expression : select.groups.group_expressions) {
				Expression(expression, ctes);
			}
			Expression(select.where_clause, ctes);
			Expression(select.having, ctes);
			Expression(select.qualify, ctes);
			if (select.from_table) {
				Ref(*select.from_table, ctes);
			}
			break;
		}
		case QueryNodeType::SET_OPERATION_NODE:
			for (auto &child : node.Cast<duckdb::SetOperationNode>().children) {
				Node(*child, ctes);
			}
			break;
		case QueryNodeType::RECURSIVE_CTE_NODE: {
			auto &recursive = node.Cast<duckdb::RecursiveCTENode>();
			ctes.push_back(Lower(Name(recursive.ctename)));
			Node(*recursive.left, ctes);
			Node(*recursive.right, ctes);
			break;
		}
		case QueryNodeType::CTE_NODE: {
			auto &cte = node.Cast<duckdb::CTENode>();
			ctes.push_back(Lower(Name(cte.ctename)));
			Node(*cte.query, ctes);
			Node(*cte.child, ctes);
			break;
		}
		default:
			// INSERT … RETURNING and the like in a FROM: not a SELECT.
			Violate("V2", "", "разрешены только SELECT, WITH, UNION, EXCEPT, INTERSECT и VALUES");
		}
		ParsedExpressionIterator::EnumerateQueryNodeModifiers(
		    node, [&](duckdb::unique_ptr<ParsedExpression> &child) { Expression(child, ctes); });
	}

	void Ref(TableRef &ref, const std::vector<std::string> &ctes) {
		switch (ref.type) {
		case TableReferenceType::BASE_TABLE:
			BaseTable(ref.Cast<duckdb::BaseTableRef>(), ctes);
			break;
		case TableReferenceType::SUBQUERY:
			Node(*ref.Cast<duckdb::SubqueryRef>().subquery->node, ctes);
			break;
		case TableReferenceType::JOIN: {
			auto &join = ref.Cast<duckdb::JoinRef>();
			Ref(*join.left, ctes);
			Ref(*join.right, ctes);
			Expression(join.condition, ctes);
			break;
		}
		case TableReferenceType::TABLE_FUNCTION:
			TableFunction(ref.Cast<duckdb::TableFunctionRef>(), ctes);
			break;
		case TableReferenceType::EXPRESSION_LIST:
			for (auto &row : ref.Cast<duckdb::ExpressionListRef>().values) {
				for (auto &value : row) {
					Expression(value, ctes);
				}
			}
			break;
		case TableReferenceType::PIVOT: {
			auto &pivot = ref.Cast<duckdb::PivotRef>();
			Ref(*pivot.source, ctes);
			for (auto &aggregate : pivot.aggregates) {
				Expression(aggregate, ctes);
			}
			for (auto &column : pivot.pivots) {
				for (auto &expression : column.pivot_expressions) {
					Expression(expression, ctes);
				}
				for (auto &entry : column.entries) {
					Expression(entry.expr, ctes);
				}
				if (column.subquery) {
					Node(*column.subquery, ctes);
				}
			}
			break;
		}
		case TableReferenceType::EMPTY_FROM:
			break;
		default:
			// SHOW, SUMMARIZE, DESCRIBE and internal references read tables of their own.
			Violate("V2", ref.ToString(), "в FROM разрешены таблицы, подзапросы, JOIN, VALUES и unnest/range/generate_series");
		}
	}

	void BaseTable(duckdb::BaseTableRef &table, const std::vector<std::string> &ctes) {
		const auto &qualified = table.GetQualifiedName();
		const auto catalog = Name(qualified.Catalog());
		const auto schema = Name(qualified.Schema());
		const auto name = Name(table.Table());
		const auto written = (catalog.empty() ? "" : catalog + ".") + (schema.empty() ? "" : schema + ".") + name;
		// V5 first: a quoted path is a table name to the parser.
		if (name.find_first_of("./\\:") != std::string::npos || (!name.empty() && name[0] == '\'')) {
			Violate("V5", written, "имя таблицы похоже на путь или URL; читать файлы из Local SQL нельзя");
		}
		if (!catalog.empty()) {
			Violate("V4", written, "указывать каталог (db.schema.table) нельзя");
		}
		if (!schema.empty()) {
			if (Lower(schema) != "xl_import" || !Contains(scope.imports, name)) {
				Violate("V4", written,
				        "со схемой разрешена только таблица xl_import.<имя>, объявленная в @depends: xl_import.<имя>");
			}
			return;
		}
		if (!Contains(ctes, name) && !Contains(scope.queries, name) && !Contains(scope.imports, name)) {
			Violate("V3", written, "таблица не объявлена в @depends и не является CTE этого запроса");
		}
	}

	void TableFunction(duckdb::TableFunctionRef &ref, const std::vector<std::string> &ctes) {
		if (!ref.function || ref.function->GetExpressionClass() != ExpressionClass::FUNCTION) {
			Violate("V6", ref.ToString(), "табличные функции разрешены только unnest, range и generate_series");
		}
		auto &function = ref.function->Cast<duckdb::FunctionExpression>();
		const auto &qualified = function.GetQualifiedName();
		const auto name = Lower(Name(function.FunctionName()));
		const bool allowed = std::find(std::begin(ALLOWED_TABLE_FUNCTIONS), std::end(ALLOWED_TABLE_FUNCTIONS), name) !=
		                     std::end(ALLOWED_TABLE_FUNCTIONS);
		if (!allowed || !Name(qualified.Schema()).empty() || !Name(qualified.Catalog()).empty()) {
			Violate("V6", Name(function.FunctionName()),
			        "табличные функции разрешены только unnest, range и generate_series");
		}
		// Their arguments are expressions like any other.
		ParsedExpressionIterator::EnumerateChildren(*ref.function,
		                                            [&](ParsedExpression &child) { ExpressionTree(child, ctes); });
		if (ref.subquery) {
			Node(*ref.subquery->node, ctes);
		}
	}

	void Expression(duckdb::unique_ptr<ParsedExpression> &expression, const std::vector<std::string> &ctes) {
		if (expression) {
			ExpressionTree(*expression, ctes);
		}
	}

	void ExpressionTree(ParsedExpression &expression, const std::vector<std::string> &ctes) {
		switch (expression.GetExpressionClass()) {
		case ExpressionClass::FUNCTION:
			ScalarFunction(expression.Cast<duckdb::FunctionExpression>());
			break;
		case ExpressionClass::PARAMETER:
			Parameter(expression.Cast<duckdb::ParameterExpression>());
			break;
		case ExpressionClass::SUBQUERY:
			Node(*expression.Cast<duckdb::SubqueryExpression>().SubqueryMutable()->node, ctes);
			break;
		default:
			break;
		}
		ParsedExpressionIterator::EnumerateChildren(expression,
		                                            [&](ParsedExpression &child) { ExpressionTree(child, ctes); });
	}

	void ScalarFunction(duckdb::FunctionExpression &function) {
		const auto name = Lower(Name(function.FunctionName()));
		for (const auto *forbidden : FORBIDDEN_FUNCTIONS) {
			if (name == forbidden) {
				Violate("V7", Name(function.FunctionName()), "функция запрещена в Local SQL");
			}
		}
		for (const auto *prefix : FORBIDDEN_PREFIXES) {
			if (name.compare(0, std::char_traits<char>::length(prefix), prefix) == 0) {
				Violate("V7", Name(function.FunctionName()), "функции ofexcel_*, fusion_scanner_*, oracle_fusion_* запрещены");
			}
		}
	}

	void Parameter(duckdb::ParameterExpression &parameter) {
		const auto identifier = Name(parameter.Identifier());
		// ? and $1 are numbered; a declared @param has a name.
		const bool positional =
		    identifier.empty() || std::all_of(identifier.begin(), identifier.end(),
		                                      [](unsigned char c) { return std::isdigit(c) != 0; });
		if (positional) {
			Violate("V8", identifier.empty() ? "?" : "$" + identifier,
			        "позиционные параметры (? и $1) запрещены; используйте $имя из @param");
		}
		if (!Contains(scope.params, identifier)) {
			Violate("V8", "$" + identifier, "параметр не объявлен в @param");
		}
	}

	const LocalSqlScope &scope;
};

} // namespace

LocalSqlScope LocalSqlScope::FromHeader(const QueryHeader &header) {
	LocalSqlScope scope;
	for (const auto &dependency : header.depends) {
		(dependency.kind == Dependency::Kind::IMPORT ? scope.imports : scope.queries).push_back(Lower(dependency.name));
	}
	for (const auto &param : header.params) {
		scope.params.push_back(Lower(param.name));
	}
	return scope;
}

LocalSqlScope LocalSqlScope::FromLists(const std::vector<std::string> &depends, const std::vector<std::string> &params) {
	LocalSqlScope scope;
	for (const auto &entry : depends) {
		auto name = Lower(entry.substr(0, entry.find('(')));
		while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
			name.pop_back();
		}
		const std::string prefix = "xl_import.";
		if (name.compare(0, prefix.size(), prefix) == 0) {
			scope.imports.push_back(name.substr(prefix.size()));
		} else {
			scope.queries.push_back(name);
		}
	}
	for (const auto &param : params) {
		scope.params.push_back(Lower(param));
	}
	return scope;
}

LocalSqlVerdict ValidateLocalSql(const std::string &sql, const LocalSqlScope &scope) {
	LocalSqlVerdict verdict;
	// The built-in grammar: what an extension adds to the parser is not Local SQL.
	auto parser = duckdb::Parser::GetBuiltinParser();
	try {
		parser.ParseQuery(sql);
	} catch (const std::exception &error) {
		verdict.code = "E_SQL_COMPILE";
		verdict.message = "Local SQL не разбирается: " + duckdb::ErrorData(error).Message();
		return verdict;
	}
	try {
		if (parser.statements.size() != 1) {
			Violate("V1", std::to_string(parser.statements.size()),
			        "нужна ровно одна инструкция, а их " + std::to_string(parser.statements.size()));
		}
		Validator(scope).Statement(*parser.statements[0]);
	} catch (const Violation &violation) {
		verdict.code = "E_VALIDATION";
		verdict.rule = violation.rule;
		verdict.fragment = violation.fragment;
		verdict.message = violation.rule + ": " + violation.message + (violation.fragment.empty() ? "" : " — " + violation.fragment);
		return verdict;
	}
	verdict.ok = true;
	return verdict;
}

} // namespace ofexcel
