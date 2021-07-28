#include "./9cc.h"

//
// Parser
//

/* ノードの作成関数 */
Node *new_node(NodeKind kind) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  return node;
}

/* 二分木ノードの作成関数 */
Node *new_binary(NodeKind kind, Node *lhs, Node *rhs) {
  Node *node = new_node(kind);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

/* 整数ノードの作成関数 */
Node *new_num(int val) {
  Node *node = new_node(ND_NUM);
  node->val = val;
  return node;
}

Node *expr();
Node *equality();
Node *relational();
Node *add();
Node *mul();
Node *unary();
Node *primary();

/*
  左結合の演算子をパーズする関数
  EBNF: expr = equality
 */
Node *expr() { return equality(); }

/*
  `==`と`!=`をパースする関数
  EBNF: equality = relational ("==" relational | "!=" relational)*
 */
Node *equality() {
  Node *node = relational();

  for (;;) {
    if (consume("=="))
      node = new_binary(ND_EQ, node, relational());
    else if (consume("!="))
      node = new_binary(ND_NE, node, relational());
    else
      return node;
  }
}

/*
  大なり小なりをパースする関数
  EBNF: relational = add ("<" add | "<=" add | ">" add | ">=" add)*
 */
Node *relational() {
  Node *node = add();

  for (;;) {
    if (consume("<"))
      node = new_binary(ND_LT, node, add());
    else if (consume("<="))
      node = new_binary(ND_LE, node, add());
    else if (consume(">"))
      node = new_binary(ND_LT, add(), node);
    else if (consume(">="))
      node = new_binary(ND_LE, add(), node);
    else
      return node;
  }
}

/*
  大なり小なりをパースする関数
  EBNF: add = mul ("+" mul | "-" mul)*
 */
Node *add() {
  Node *node = mul();

  for (;;) {
    if (consume("+"))
      node = new_binary(ND_ADD, node, mul());
    else if (consume("-"))
      node = new_binary(ND_SUB, node, mul());
    else
      return node;
  }
}

/*
  乗除演算子をパースする関数
  EBNF: mul = unary ("*" unary | "/" unary)*
 */
Node *mul() {
  Node *node = unary();

  for (;;) {
    // gen関数で演算子のアセンブリを生成しているため、ここでは構文木のみを作成している
    if (consume("*"))
      node = new_binary(ND_MUL, node, unary());
    else if (consume("/"))
      node = new_binary(ND_DIV, node, unary());
    else
      return node;
  }
}

/* 単項演算子をパースする関数
  EBNF: unary   = ("+" | "-")? punary
*/
Node *unary() {
  if (consume("+")) return unary();  // +xをxに置換
  if (consume("-"))
    return new_binary(ND_SUB, new_num(0), unary());  // -xを0 - xに置換
  return primary();
}

/*
  算術優先記号`()`のパースする関数
  EBNF: primary = "(" expr ")" | num
 */
Node *primary() {
  // 次のトークンが"("なら、"(" expr ")"のはず
  if (consume("(")) {
    Node *node = expr();
    expect(")");
    return node;
  }

  // そうでなければ数値のはず
  return new_num(expect_number());
}
