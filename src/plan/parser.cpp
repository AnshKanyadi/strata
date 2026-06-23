#include "strata/plan/parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "strata/common/error.hpp"
#include "strata/exec/expression.hpp"
#include "strata/util/date.hpp"

namespace strata {
namespace {

// Parsing is the setup boundary (ADR 0002), so it may use exceptions: a parse
// error throws ParseFail, caught once in ParseSql and converted to a Result.
struct ParseFail {
  std::string msg;
};
[[noreturn]] void Fail(std::string msg) { throw ParseFail{std::move(msg)}; }

// --- Lexer ------------------------------------------------------------------

enum class TK { kIdent, kNumber, kString, kDateLit, kSym, kEnd };
struct Token {
  TK kind;
  std::string text;  // ident/number/string content/symbol
};

bool IsIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool IsIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::vector<Token> Tokenize(std::string_view s) {
  std::vector<Token> out;
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (IsIdentStart(c)) {
      const std::size_t start = i;
      while (i < s.size() && IsIdentChar(s[i])) ++i;
      out.push_back({TK::kIdent, std::string(s.substr(start, i - start))});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      const std::size_t start = i;
      while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' ||
                              s[i] == 'e' || s[i] == 'E' ||
                              ((s[i] == '+' || s[i] == '-') && i > start &&
                               (s[i - 1] == 'e' || s[i - 1] == 'E')))) {
        ++i;
      }
      out.push_back({TK::kNumber, std::string(s.substr(start, i - start))});
      continue;
    }
    if (c == '\'') {
      ++i;  // opening quote
      std::string lit;
      while (i < s.size() && s[i] != '\'') lit.push_back(s[i++]);
      if (i >= s.size()) Fail("unterminated string literal");
      ++i;  // closing quote
      out.push_back({TK::kString, std::move(lit)});
      continue;
    }
    // two-char then one-char symbols
    if (i + 1 < s.size()) {
      const std::string_view two = s.substr(i, 2);
      if (two == "<=" || two == ">=" || two == "<>" || two == "!=") {
        out.push_back({TK::kSym, std::string(two)});
        i += 2;
        continue;
      }
    }
    out.push_back({TK::kSym, std::string(1, c)});
    ++i;
  }
  out.push_back({TK::kEnd, {}});
  return out;
}

// --- helpers ----------------------------------------------------------------

std::string Upper(std::string_view s) {
  std::string r(s);
  for (char& ch : r) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return r;
}

Value ParseDateLiteral(const std::string& lit) {
  // strict YYYY-MM-DD
  const auto d1 = lit.find('-');
  const auto d2 = (d1 == std::string::npos) ? d1 : lit.find('-', d1 + 1);
  if (d1 == std::string::npos || d2 == std::string::npos) Fail("bad DATE literal: " + lit);
  auto to_int = [&](std::string_view sv) {
    int v = 0;
    const auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
    if (ec != std::errc{} || p != sv.data() + sv.size()) Fail("bad DATE literal: " + lit);
    return v;
  };
  const std::string_view all = lit;
  const int y = to_int(all.substr(0, d1));
  const int m = to_int(all.substr(d1 + 1, d2 - d1 - 1));
  const int d = to_int(all.substr(d2 + 1));
  return Value::Date(DaysFromCivil(y, m, d));
}

// --- Parser -----------------------------------------------------------------

class Parser {
 public:
  explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

  PlanPtr ParseStatement() {
    PlanPtr plan = ParseSelect();
    if (Cur().kind != TK::kEnd) Fail("unexpected trailing tokens near '" + Cur().text + "'");
    return plan;
  }

 private:
  const Token& Cur() const { return toks_[pos_]; }
  const Token& Peek(std::size_t n = 1) const {
    return toks_[std::min(pos_ + n, toks_.size() - 1)];
  }
  void Advance() {
    if (pos_ + 1 < toks_.size()) ++pos_;
  }
  bool IsKw(const Token& t, const char* kw) const {
    return t.kind == TK::kIdent && Upper(t.text) == kw;
  }
  bool AtKw(const char* kw) const { return IsKw(Cur(), kw); }
  bool AcceptKw(const char* kw) {
    if (AtKw(kw)) {
      Advance();
      return true;
    }
    return false;
  }
  void ExpectKw(const char* kw) {
    if (!AcceptKw(kw)) Fail(std::string("expected '") + kw + "' near '" + Cur().text + "'");
  }
  bool AtSym(const char* s) const { return Cur().kind == TK::kSym && Cur().text == s; }
  bool AcceptSym(const char* s) {
    if (AtSym(s)) {
      Advance();
      return true;
    }
    return false;
  }
  void ExpectSym(const char* s) {
    if (!AcceptSym(s)) Fail(std::string("expected '") + s + "' near '" + Cur().text + "'");
  }
  std::string ExpectIdent() {
    if (Cur().kind != TK::kIdent) Fail("expected identifier near '" + Cur().text + "'");
    std::string id = Cur().text;
    Advance();
    return id;
  }

  bool IsAggName(const std::string& up) const {
    return up == "COUNT" || up == "SUM" || up == "MIN" || up == "MAX" || up == "AVG";
  }

  struct SelectItem {
    bool is_agg = false;
    bool count_star = false;
    AggFunc func{};
    ExprPtr expr;     // scalar item
    ExprPtr agg_arg;  // aggregate argument (null for COUNT(*))
    std::string alias;
  };

  PlanPtr ParseSelect() {
    ExpectKw("SELECT");
    bool star = false;
    std::vector<SelectItem> items;
    if (AtSym("*") && IsKw(Peek(), "FROM")) {
      Advance();  // consume '*'
      star = true;
    } else {
      items.push_back(ParseSelectItem());
      while (AcceptSym(",")) items.push_back(ParseSelectItem());
    }
    ExpectKw("FROM");
    const std::string table = ExpectIdent();

    PlanPtr node = MakeNode(LogicalOp::kGet);
    node->table = table;

    if (AcceptKw("WHERE")) {
      PlanPtr f = MakeNode(LogicalOp::kFilter);
      f->predicate = ParseExpr();
      f->children.push_back(std::move(node));
      node = std::move(f);
    }

    std::vector<ExprPtr> group_keys;
    bool has_group = false;
    if (AcceptKw("GROUP")) {
      ExpectKw("BY");
      group_keys = ParseExprList();
      has_group = true;
    }

    bool any_agg = false;
    for (const SelectItem& it : items) any_agg = any_agg || it.is_agg;

    if (any_agg || has_group) {
      if (star) Fail("SELECT * not allowed with aggregation");
      PlanPtr agg = MakeNode(LogicalOp::kAggregate);
      agg->group_keys = std::move(group_keys);
      for (SelectItem& it : items) {
        if (it.is_agg) {
          agg->aggregates.push_back({it.func, std::move(it.agg_arg),
                                     it.alias.empty() ? std::string{} : it.alias});
        }
        // scalar items in an aggregate query are the group keys (taken from GROUP BY).
      }
      agg->children.push_back(std::move(node));
      node = std::move(agg);
    } else {
      PlanPtr proj = MakeNode(LogicalOp::kProject);
      proj->project_all = star;
      for (SelectItem& it : items) {
        proj->projections.push_back(std::move(it.expr));
        proj->proj_names.push_back(it.alias);
      }
      proj->children.push_back(std::move(node));
      node = std::move(proj);
    }

    if (AcceptKw("ORDER")) {
      ExpectKw("BY");
      PlanPtr ord = MakeNode(LogicalOp::kOrder);
      ord->order = ParseOrderList();
      ord->children.push_back(std::move(node));
      node = std::move(ord);
    }
    if (AcceptKw("LIMIT")) {
      PlanPtr lim = MakeNode(LogicalOp::kLimit);
      lim->limit = ParseUInt();
      lim->children.push_back(std::move(node));
      node = std::move(lim);
    }
    return node;
  }

  SelectItem ParseSelectItem() {
    SelectItem item;
    if (Cur().kind == TK::kIdent && IsAggName(Upper(Cur().text)) && Peek().kind == TK::kSym &&
        Peek().text == "(") {
      const std::string up = Upper(Cur().text);
      Advance();           // function name
      ExpectSym("(");
      item.is_agg = true;
      if (up == "COUNT") {
        if (AcceptSym("*")) {
          item.func = AggFunc::kCountStar;
          item.count_star = true;
        } else {
          item.func = AggFunc::kCount;
          item.agg_arg = ParseExpr();
        }
      } else {
        item.func = (up == "SUM")   ? AggFunc::kSum
                    : (up == "MIN") ? AggFunc::kMin
                    : (up == "MAX") ? AggFunc::kMax
                                    : AggFunc::kAvg;
        item.agg_arg = ParseExpr();
      }
      ExpectSym(")");
    } else {
      item.expr = ParseExpr();
    }
    // optional alias: [AS] ident
    if (AcceptKw("AS")) {
      item.alias = ExpectIdent();
    } else if (Cur().kind == TK::kIdent && !IsKeywordBoundary()) {
      item.alias = ExpectIdent();
    }
    return item;
  }

  // After an item, a bare identifier is an alias unless it's a clause keyword.
  bool IsKeywordBoundary() const {
    const std::string up = Upper(Cur().text);
    return up == "FROM" || up == "WHERE" || up == "GROUP" || up == "ORDER" || up == "LIMIT" ||
           up == "AS" || up == "AND" || up == "OR" || up == "ASC" || up == "DESC" ||
           up == "NULLS" || up == "BETWEEN";
  }

  std::vector<ExprPtr> ParseExprList() {
    std::vector<ExprPtr> v;
    v.push_back(ParseExpr());
    while (AcceptSym(",")) v.push_back(ParseExpr());
    return v;
  }

  std::vector<OrderItem> ParseOrderList() {
    std::vector<OrderItem> v;
    auto one = [&]() {
      OrderItem o;
      o.expr = ParseExpr();
      if (AcceptKw("ASC")) o.ascending = true;
      else if (AcceptKw("DESC")) o.ascending = false;
      if (AcceptKw("NULLS")) {
        if (AcceptKw("FIRST")) o.nulls_first = true;
        else if (AcceptKw("LAST")) o.nulls_first = false;
        else Fail("expected FIRST or LAST after NULLS");
      }
      v.push_back(std::move(o));
    };
    one();
    while (AcceptSym(",")) one();
    return v;
  }

  std::size_t ParseUInt() {
    if (Cur().kind != TK::kNumber) Fail("expected integer after LIMIT");
    std::size_t v = 0;
    const std::string& t = Cur().text;
    const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), v);
    if (ec != std::errc{} || p != t.data() + t.size()) Fail("bad LIMIT value: " + t);
    Advance();
    return v;
  }

  // expression grammar (precedence climbing)
  ExprPtr ParseExpr() { return ParseOr(); }

  ExprPtr ParseOr() {
    ExprPtr e = ParseAnd();
    while (AcceptKw("OR")) e = Expression::Or(std::move(e), ParseAnd());
    return e;
  }
  ExprPtr ParseAnd() {
    ExprPtr e = ParseNot();
    while (AcceptKw("AND")) e = Expression::And(std::move(e), ParseNot());
    return e;
  }
  ExprPtr ParseNot() {
    if (AcceptKw("NOT")) return Expression::Not(ParseNot());
    return ParseComparison();
  }
  ExprPtr ParseComparison() {
    ExprPtr e = ParseAdditive();
    if (AcceptKw("BETWEEN")) {
      ExprPtr lo = ParseAdditive();
      ExpectKw("AND");
      ExprPtr hi = ParseAdditive();
      // desugar: e >= lo AND e <= hi  (clone e for the second comparison)
      ExprPtr ge = Expression::Comparison(simd::CmpOp::kGe, Clone(*e), std::move(lo));
      ExprPtr le = Expression::Comparison(simd::CmpOp::kLe, std::move(e), std::move(hi));
      return Expression::And(std::move(ge), std::move(le));
    }
    simd::CmpOp op;
    if (AcceptSym("=")) op = simd::CmpOp::kEq;
    else if (AcceptSym("<>") || AcceptSym("!=")) op = simd::CmpOp::kNe;
    else if (AcceptSym("<=")) op = simd::CmpOp::kLe;
    else if (AcceptSym(">=")) op = simd::CmpOp::kGe;
    else if (AcceptSym("<")) op = simd::CmpOp::kLt;
    else if (AcceptSym(">")) op = simd::CmpOp::kGt;
    else return e;
    return Expression::Comparison(op, std::move(e), ParseAdditive());
  }
  ExprPtr ParseAdditive() {
    ExprPtr e = ParseMultiplicative();
    for (;;) {
      if (AcceptSym("+")) e = Expression::Arithmetic(simd::ArithOp::kAdd, std::move(e), ParseMultiplicative());
      else if (AcceptSym("-")) e = Expression::Arithmetic(simd::ArithOp::kSub, std::move(e), ParseMultiplicative());
      else break;
    }
    return e;
  }
  ExprPtr ParseMultiplicative() {
    ExprPtr e = ParsePrimary();
    while (AcceptSym("*")) e = Expression::Arithmetic(simd::ArithOp::kMul, std::move(e), ParsePrimary());
    return e;
  }
  ExprPtr ParsePrimary() {
    if (AcceptSym("(")) {
      ExprPtr e = ParseExpr();
      ExpectSym(")");
      return e;
    }
    if (Cur().kind == TK::kNumber) {
      const std::string t = Cur().text;
      Advance();
      if (t.find('.') != std::string::npos || t.find('e') != std::string::npos ||
          t.find('E') != std::string::npos) {
        double d = 0;
        std::from_chars(t.data(), t.data() + t.size(), d);
        return Expression::Constant(Value::Double(d));
      }
      std::int64_t v = 0;
      std::from_chars(t.data(), t.data() + t.size(), v);
      if (v >= -2147483648LL && v <= 2147483647LL) {
        return Expression::Constant(Value::Int32(static_cast<std::int32_t>(v)));
      }
      return Expression::Constant(Value::Int64(v));
    }
    if (Cur().kind == TK::kString) {
      const std::string lit = Cur().text;
      Advance();
      return Expression::Constant(Value::Varchar(lit));
    }
    if (AtKw("DATE")) {
      Advance();
      if (Cur().kind != TK::kString) Fail("expected string after DATE");
      const std::string lit = Cur().text;
      Advance();
      return Expression::Constant(ParseDateLiteral(lit));
    }
    if (AtKw("NULL")) {
      Advance();
      return Expression::Constant(Value::Null(TypeId::kInt32));  // type fixed by the binder
    }
    if (AtKw("TRUE") || AtKw("FALSE")) {
      const bool b = AtKw("TRUE");
      Advance();
      return Expression::Constant(Value::Bool(b));
    }
    if (Cur().kind == TK::kIdent) {
      std::string name = ExpectIdent();
      if (AcceptSym(".")) {
        std::string col = ExpectIdent();
        return Expression::ColumnRefByName(std::move(col), std::move(name));
      }
      return Expression::ColumnRefByName(std::move(name));
    }
    Fail("unexpected token '" + Cur().text + "' in expression");
  }

  // Deep-clone an unbound expression (for BETWEEN desugaring).
  static ExprPtr Clone(const Expression& e) {
    auto c = std::make_unique<Expression>();
    c->kind = e.kind;
    c->type = e.type;
    c->column_index = e.column_index;
    c->column_name = e.column_name;
    c->table_qualifier = e.table_qualifier;
    c->constant = e.constant;
    c->cmp = e.cmp;
    c->arith = e.arith;
    for (const ExprPtr& ch : e.children) c->children.push_back(Clone(*ch));
    return c;
  }

  std::vector<Token> toks_;
  std::size_t pos_ = 0;
};

}  // namespace

Result<PlanPtr> ParseSql(std::string_view sql) {
  try {
    Parser p(Tokenize(sql));
    return p.ParseStatement();
  } catch (const ParseFail& e) {
    return Err(ErrorCode::kParseError, e.msg);
  }
}

}  // namespace strata
