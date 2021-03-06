#include "./9cc.h"

//
// 注釈：
// tokenで分割した文字列を、構造体を利用して抽象構文木にする
// gen関数で演算子のアセンブリを生成しているため、ここでは構文木のみを作成
//

// 解析中に作成されたすべてのローカル変数インスタンスは
// この配列に蓄積されます。
static VarList *locals;
static VarList *globals;
static VarList *scope;

// Find a local variable by name.
static Var *find_var(Token *tok) {
  for (VarList *vl = scope; vl; vl = vl->next) {
    Var *var = vl->var;
    if (strlen(var->name) == tok->len &&
        !strncmp(tok->str, var->name, tok->len))
      return var;
  }
  return NULL;
}

/* ノードの作成関数 */
static Node *new_node(NodeKind kind, Token *tok) {
  Node *node = calloc(1, sizeof(Node));
  node->kind = kind;
  node->tok = tok;
  return node;
}

/* 二分木ノードの作成関数 */
static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = lhs;
  node->rhs = rhs;
  return node;
}

/* 左しかない木ノードの作成関数 */
static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
  Node *node = new_node(kind, tok);
  node->lhs = expr;
  return node;
}

/* 整数ノードの作成関数 */
static Node *new_num(long val, Token *tok) {
  Node *node = new_node(ND_NUM, tok);
  node->val = val;
  return node;
}

/* 変数ノード作成関数 */
static Node *new_var_node(Var *var, Token *tok) {
  Node *node = new_node(ND_VAR, tok);
  node->var = var;
  return node;
}

/* 変数のノード作成関数 */
static Var *new_var(char *name, Type *ty, bool is_local) {
  Var *var = calloc(1, sizeof(Var));  // Varの構造体一つずつに1byteメモリを確保
  var->name = name;
  var->ty = ty;
  var->is_local = is_local;

  VarList *sc = calloc(1, sizeof(VarList));
  sc->var = var;
  sc->next = scope;
  scope = sc;
  return var;
}

/* ローカル変数専用のノード作成関数 */
static Var *new_lvar(char *name, Type *ty) {
  Var *var = new_var(name, ty, true);

  var->name = name;
  var->ty = ty;

  VarList *vl = calloc(1, sizeof(VarList));
  vl->var = var;
  vl->next = locals;
  locals = vl;
  return var;
}

static Var *new_gvar(char *name, Type *ty) {
  Var *var = new_var(name, ty, false);

  VarList *vl = calloc(1, sizeof(VarList));
  vl->var = var;
  vl->next = globals;
  globals = vl;
  return var;
}

static char *new_label(void) {
  static int cnt = 0;
  char buf[20];
  sprintf(buf, ".L.data.%d", cnt++);
  return strndup(buf, 20);
}

// forward declaration

static Function *function(void);
static Type *basetype(void);
static Type *struct_decl(void);
static Member *struct_member(void);
static void global_var(void);
static Node *declaration(void);
static bool is_typename(void);
static Node *stmt(void);
static Node *stmt2(void);
static Node *expr(void);
static Node *assign(void);
static Node *equality(void);
static Node *relational(void);
static Node *add(void);
static Node *mul(void);
static Node *unary(void);
static Node *postfix(void);
static Node *primary(void);

/*
  次のトップレベルの項目が関数かグローバル変数かを、入力トークンを先読みして判断します。
 */
static bool is_function(void) {
  Token *tok = token;
  basetype();
  bool isfunc = consume_ident() && consume("(");
  token = tok;
  return isfunc;
}

/*
  複数行プログラム全体をパースする関数
  EBNF: program = (global-var | function)*
 */
Program *program(void) {
  Function head = {};
  Function *cur = &head;
  globals = NULL;

  while (!at_eof()) {
    if (is_function()) {
      cur->next = function();
      cur = cur->next;
    } else {
      global_var();
    }
  }

  Program *prog = calloc(1, sizeof(Program));
  prog->globals = globals;
  prog->fns = head.next;
  return prog;
}

// basetype = ("char" | "int" | struct-decl) "*"*
static Type *basetype(void) {
  if (!is_typename()) error_tok(token, "typename expected");

  Type *ty;
  if (consume("char"))
    ty = char_type;
  else if (consume("int"))
    ty = int_type;
  else
    ty = struct_decl();

  while (consume("*")) ty = pointer_to(ty);
  return ty;
}

/* 次のトークンが[でない(つまり整数)ならそのまま、配列ならarray_ofを適用 */
static Type *read_type_suffix(Type *base) {
  if (!consume("[")) return base;
  int sz = expect_number();
  expect("]");
  base = read_type_suffix(base);
  return array_of(base, sz);
}

// struct-decl = "struct" "{" struct-member "}"
static Type *struct_decl(void) {
  // Read struct members.
  expect("struct");
  expect("{");

  Member head = {};
  Member *cur = &head;

  while (!consume("}")) {
    cur->next = struct_member();
    cur = cur->next;
  }

  Type *ty = calloc(1, sizeof(Type));
  ty->kind = TY_STRUCT;
  ty->members = head.next;

  // Assign offsets within the struct to members.
  int offset = 0;
  for (Member *mem = ty->members; mem; mem = mem->next) {
    mem->offset = offset;
    offset += mem->ty->size;
  }
  ty->size = offset;

  return ty;
}

// struct-member = basetype ident ("[" num "]")* ";"
static Member *struct_member(void) {
  Member *mem = calloc(1, sizeof(Member));
  mem->ty = basetype();
  mem->name = expect_ident();
  mem->ty = read_type_suffix(mem->ty);
  expect(";");
  return mem;
}

static VarList *read_func_param(void) {
  Type *ty = basetype();
  char *name = expect_ident();
  ty = read_type_suffix(ty);

  VarList *vl = calloc(1, sizeof(VarList));
  vl->var = new_lvar(name, ty);
  return vl;
}

/* 引数が0個ならNULLを返却、あれば変数ノード(VarList)を作成 */
static VarList *read_func_params(void) {
  if (consume(")")) return NULL;

  VarList *head = read_func_param();

  VarList *cur = head;

  while (!consume(")")) {
    expect(",");
    cur->next = read_func_param();
    cur = cur->next;
  }

  return head;
}

// function = basetype ident "(" params? ")" "{" stmt* "}"
// params   = param ("," param)*
// param    = basetype ident
static Function *function(void) {
  locals = NULL;

  Function *fn = calloc(1, sizeof(Function));
  basetype();
  fn->name = expect_ident();
  expect("(");

  VarList *sc = scope;
  fn->params = read_func_params();
  expect("{");

  Node head = {};
  Node *cur = &head;

  while (!consume("}")) {
    cur->next = stmt();
    cur = cur->next;
  }
  scope = sc;

  fn->node = head.next;
  fn->locals = locals;
  return fn;
}

// global-var = basetype ident ("[" num "]")* ";"
static void global_var(void) {
  Type *ty = basetype();
  char *name = expect_ident();
  ty = read_type_suffix(ty);
  expect(";");
  new_gvar(name, ty);
}

// 変数宣言
// declaration = basetype ident ("[" num "]")* ("=" expr) ";"
static Node *declaration(void) {
  Token *tok = token;
  Type *ty = basetype();
  char *name = expect_ident();
  ty = read_type_suffix(ty);
  Var *var = new_lvar(name, ty);

  if (consume(";")) return new_node(ND_NULL, tok);

  expect("=");
  Node *lhs = new_var_node(var, tok);
  Node *rhs = expr();
  expect(";");
  Node *node = new_binary(ND_ASSIGN, lhs, rhs, tok);
  return new_unary(ND_EXPR_STMT, node, tok);
}

static Node *read_expr_stmt(void) {
  Token *tok = token;
  return new_unary(ND_EXPR_STMT, expr(), tok);
}

// 次のトークンが型を表す場合は、trueを返します。
static bool is_typename(void) {
  return peek("char") || peek("int") || peek("struct");
}

/* 渡されたノードに型ノードを追加する処理を挟む */
static Node *stmt(void) {
  Node *node = stmt2();
  add_type(node);
  return node;
}

/*
  予約語と、行の区切り文字`;`をパースする関数
  EBNF: stmt = "return" expr ";"
              | "if" "(" expr ")" stmt ("else" stmt)?
              | "while" "(" expr ")" stmt
              | "for" "(" expr? ";" expr? ";" expr? ")" stmt
              | "{" stmt* "}"
              | declaration
              | expr ";"
 */
static Node *stmt2(void) {
  Token *tok;
  if (tok = consume("return")) {
    Node *node = new_unary(ND_RETURN, expr(), tok);
    expect(";");
    return node;
  }

  if (tok = consume("if")) {
    Node *node = new_node(ND_IF, tok);
    expect("(");
    node->cond = expr();
    expect(")");
    node->then = stmt();
    if (consume("else")) node->els = stmt();
    return node;
  }

  if (tok = consume("while")) {
    Node *node = new_node(ND_WHILE, tok);
    expect("(");
    node->cond = expr();
    expect(")");
    node->then = stmt();
    return node;
  }

  if (tok = consume("for")) {
    Node *node = new_node(ND_FOR, tok);
    expect("(");

    // "for"初期値構文の始めに";"が来ていないかを確かめることで、
    // EBNFの"?"というオプショナルを実現している
    if (!consume(";")) {  // "for"の初期値
      node->init = read_expr_stmt();
      expect(";");
    }
    if (!consume(";")) {  // "for"の条件部分
      node->cond = expr();
      expect(";");
    }
    if (!consume(")")) {  // "for"の累積量部分
      node->inc = read_expr_stmt();
      expect(")");
    }
    node->then = stmt();
    return node;
  }

  VarList *sc = scope;
  if (tok = consume("{")) {
    Node head = {};
    Node *cur = &head;

    while (!consume("}")) {
      cur->next = stmt();
      cur = cur->next;
    }
    scope = sc;

    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    return node;
  }

  if (is_typename()) return declaration();

  Node *node = read_expr_stmt();
  expect(";");
  return node;
}

/*
  assign演算子をパースする関数
  EBNF: expr = assign
 */
static Node *expr(void) { return assign(); }

/*
  `=`演算子をパースする関数
  EBNF: assign = equality ("=" assign)?
 */
static Node *assign(void) {
  Node *node = equality();
  Token *tok;
  if (consume("=")) node = new_binary(ND_ASSIGN, node, assign(), tok);
  return node;
}

/*
  比較演算子の`==`と`!=`をパースする関数
  EBNF: equality = relational ("==" relational | "!=" relational)*
 */
static Node *equality(void) {
  Node *node = relational();
  Token *tok;

  for (;;) {
    if (tok = consume("=="))
      node = new_binary(ND_EQ, node, relational(), tok);
    else if (tok = consume("!="))
      node = new_binary(ND_NE, node, relational(), tok);
    else
      return node;
  }
}

/*
  比較演算子の大なり小なりをパースする関数
  EBNF: relational = add ("<" add | "<=" add | ">" add | ">=" add)*
 */
static Node *relational(void) {
  Node *node = add();
  Token *tok;

  for (;;) {
    if (tok = consume("<"))
      node = new_binary(ND_LT, node, add(), tok);
    else if (tok = consume("<="))
      node = new_binary(ND_LE, node, add(), tok);
    else if (tok = consume(">"))
      node = new_binary(ND_LT, add(), node, tok);
    else if (tok = consume(">="))
      node = new_binary(ND_LE, add(), node, tok);
    else
      return node;
  }
}

/* 整数同士の足し算、ポインタの足し算ノードを作成する関数 */
static Node *new_add(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_integer(lhs->ty) && is_integer(rhs->ty))
    return new_binary(ND_ADD, lhs, rhs, tok);
  if (lhs->ty->base && is_integer(rhs->ty))
    return new_binary(ND_PTR_ADD, lhs, rhs, tok);
  if (is_integer(lhs->ty) && rhs->ty->base)
    return new_binary(ND_PTR_ADD, rhs, lhs, tok);
  error_tok(tok, "invalid operands");
}

/* 整数同士の引き算、ポインタの引き算ノードを作成する関数 */
static Node *new_sub(Node *lhs, Node *rhs, Token *tok) {
  add_type(lhs);
  add_type(rhs);

  if (is_integer(lhs->ty) && is_integer(rhs->ty))
    return new_binary(ND_SUB, lhs, rhs, tok);
  if (lhs->ty->base && is_integer(rhs->ty))
    return new_binary(ND_PTR_SUB, lhs, rhs, tok);
  if (lhs->ty->base && rhs->ty->base)
    return new_binary(ND_PTR_DIFF, lhs, rhs, tok);
  error_tok(tok, "invalid operands");
}

/*
  加減演算子をパースする関数
  EBNF: add = mul ("+" mul | "-" mul)*
 */
static Node *add(void) {
  Node *node = mul();
  Token *tok;

  for (;;) {
    if (tok = consume("+"))
      node = new_add(node, mul(), tok);
    else if (tok = consume("-"))
      node = new_sub(node, mul(), tok);
    else
      return node;
  }
}

/*
  乗除演算子をパースする関数
  EBNF: mul = unary ("*" unary | "/" unary)*
 */
static Node *mul(void) {
  Node *node = unary();
  Token *tok;

  for (;;) {
    if (tok = consume("*"))
      node = new_binary(ND_MUL, node, unary(), tok);
    else if (tok = consume("/"))
      node = new_binary(ND_DIV, node, unary(), tok);
    else
      return node;
  }
}

/*
  単項演算子をパースする関数
  EBNF: unary   = ("+" | "-" | "*" | "&")? unary　
                | postfix
*/
static Node *unary(void) {
  Token *tok;
  if (consume("+")) return unary();  // +xをxに置換

  if (tok = consume("-"))  // -xを0 - xに置換
    return new_binary(ND_SUB, new_num(0, tok), unary(), tok);

  if (tok = consume("&"))  // -アドレスを取り出す
    return new_unary(ND_ADDR, unary(), tok);

  if (tok = consume("*"))  // ポインタまたはアドレスから値を取り出す
    return new_unary(ND_DEREF, unary(), tok);
  return postfix();
}

static Member *find_member(Type *ty, char *name) {
  for (Member *mem = ty->members; mem; mem = mem->next)
    if (!strcmp(mem->name, name)) return mem;
  return NULL;
}

static Node *struct_ref(Node *lhs) {
  add_type(lhs);
  if (lhs->ty->kind != TY_STRUCT) error_tok(lhs->tok, "not a struct");

  Token *tok = token;
  Member *mem = find_member(lhs->ty, expect_ident());
  if (!mem) error_tok(tok, "no such member");

  Node *node = new_unary(ND_MEMBER, lhs, tok);
  node->member = mem;
  return node;
}

// postfix = primary ("[" expr "]" | "." ident)*
static Node *postfix(void) {
  Node *node = primary();
  Token *tok;

  for (;;) {
    if (tok = consume("[")) {
      // x[y] is short for *(x+y)
      Node *exp = new_add(node, expr(), tok);
      expect("]");
      node = new_unary(ND_DEREF, exp, tok);
      continue;
    }

    if (tok = consume(".")) {
      node = struct_ref(node);
      continue;
    }

    return node;
  }
}

// stmt-expr = "(" "{" stmt stmt* "}" ")"
//
// ステートメント式は、GNU Cの拡張機能です。
static Node *stmt_expr(Token *tok) {
  Node *node = new_node(ND_STMT_EXPR, tok);
  node->body = stmt();
  Node *cur = node->body;

  while (!consume("}")) {
    cur->next = stmt();
    cur = cur->next;
  }
  expect(")");

  if (cur->kind != ND_EXPR_STMT)
    error_tok(cur->tok, "stmt expr returning void is not supported");
  memcpy(cur, cur->lhs, sizeof(Node));
  return node;
}

/*
  引数の有無に応じて処理が分岐し、パースする関数
  EBNF: func-args = "(" (assign ("," assign)*)? ")"
*/
static Node *func_args(void) {
  if (consume(")")) return NULL;

  Node *head = assign();
  Node *cur = head;
  while (consume(",")) {
    cur->next = assign();
    cur = cur->next;
  }
  expect(")");
  return head;
}

/*
  算術優先記号`()`と`関数`、`変数`、`整数`をパースする関数
  EBNF: primary = "(" "{" stmt-expr-tail
                | "(" expr ")"
                | "sizeof" unary
                | ident args?
                | str
                | num
 */
static Node *primary(void) {
  Token *tok;

  // 次のトークンが"("なら、"(" expr ")"のはず
  if (consume("(")) {
    if (consume("{")) return stmt_expr(tok);

    Node *node = expr();
    expect(")");
    return node;
  }

  if (tok = consume("sizeof")) {
    Node *node = unary();
    add_type(node);
    return new_num(node->ty->size, tok);
  }

  if (tok = consume_ident()) {
    // 識別子の次に"()"がきたら Function
    if (consume("(")) {
      Node *node = new_node(ND_FUNCALL, tok);
      // strndupは第2引数のサイズ指定分、文字列を複製する
      node->funcname = strndup(tok->str, tok->len);
      node->args = func_args();  // 引数ノードの作成は`func_args`に任せる
      return node;
    }

    Var *var = find_var(tok);
    if (!var)
      // 既存の変数名が見つからない場合、エラー
      error_tok(tok, "undefined variable");
    return new_var_node(var, tok);
  }

  tok = token;
  if (tok->kind == TK_STR) {
    token = token->next;

    Type *ty = array_of(char_type, tok->cont_len);
    Var *var = new_gvar(new_label(), ty);
    var->contents = tok->contents;
    var->cont_len = tok->cont_len;
    return new_var_node(var, tok);
  }

  if (tok->kind != TK_NUM) error_tok(tok, "expected expression");
  // そうでなければ数値のはず
  return new_num(expect_number(), tok);
}
