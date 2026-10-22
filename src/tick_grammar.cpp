#include "tick_grammar.hpp"

#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/connection_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/planner/extension_callback.hpp"

namespace duckdb {

static constexpr const char *TICK_GRAMMAR_NAME = "tick";

static bool IsStringLiteral(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::CONSTANT) {
		return false;
	}
	return expr.Cast<ConstantExpression>().GetLiteral().kind == LiteralKind::STRING;
}

//! `x IN <expr>` normally becomes contains(<expr>, x); a bare string literal becomes tick_in(x, '<pattern>')
static arena_ptr<TransformResultValue> FinalizeInContainsExpression(PEGTransformer &transformer,
                                                                    ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	auto expr = transformer.Transform<unique_ptr<ParsedExpression>>(list.GetChild(0));
	auto function_name = IsStringLiteral(*expr) ? "tick_in" : "contains";
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(expr));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(
	    make_uniq<FunctionExpression>(Identifier(function_name), std::move(children)));
}

static arena_ptr<TransformProcess> StartInContainsExpression(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, FinalizeInContainsExpression);
}

class TickGrammarExtension final : public GrammarExtension {
public:
	TickGrammarExtension()
	    : GrammarExtension(TICK_GRAMMAR_NAME, "QuestDB TICK interval syntax: x IN '<pattern>' becomes tick_in(x, ...)") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		changes.push_back(GrammarChange::SetTransformProcess("InContainsExpression", StartInContainsExpression));
		return changes;
	}
};

//! Adds the tick grammar to the connection's active grammar extensions, keeping any already active
static void ActivateTickGrammar(ClientContext &context) {
	auto &client_config = ClientConfig::GetConfig(context);
	vector<Value> names;
	for (auto &name : client_config.active_grammar_extensions) {
		if (StringUtil::CIEquals(name, TICK_GRAMMAR_NAME)) {
			return;
		}
		names.emplace_back(name);
	}
	names.emplace_back(TICK_GRAMMAR_NAME);
	ActiveGrammarExtensionsSetting::SetLocal(context, Value::LIST(LogicalType::VARCHAR, std::move(names)));
}

class TickConnectionCallback final : public ExtensionCallback {
public:
	void OnConnectionOpened(ClientContext &context) override {
		ActivateTickGrammar(context);
	}
};

//! Activates the grammar on an already open connection once its current (or next) query ends
class TickActivationState final : public ClientContextState {
public:
	void QueryEnd(ClientContext &context) override {
		if (activated) {
			return;
		}
		activated = true;
		ActivateTickGrammar(context);
	}

private:
	bool activated = false;
};

void RegisterTickGrammar(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	GrammarExtension::Register(db, make_shared_ptr<TickGrammarExtension>());
	ExtensionCallback::Register(DBConfig::GetConfig(db), make_shared_ptr<TickConnectionCallback>());
	for (auto &connection : ConnectionManager::Get(db).GetConnectionList()) {
		connection->registered_state->Insert("tick_grammar_activation", make_shared_ptr<TickActivationState>());
	}
}

} // namespace duckdb
