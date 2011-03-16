#include <sstream>

#include "Instrumentation.h"

using namespace clang;
using namespace std;


/// Explicitly cast an expression to a type (probably only works for casting
/// things to pointer types).
Expr* cast(Expr *from, QualType to, ASTContext &ast);

/// Take the address of an expression result.
Expr* addressOf(Expr*, ASTContext&, SourceLocation loc = SourceLocation());

/// Declare a function.
FunctionDecl *declareFn(const string& name, QualType returnType,
    vector<QualType>& argTypes, ASTContext &ast);

/// Declare and call a function.
Expr *call(string name, QualType returnType, vector<Expr*>& params,
    ASTContext& ast, SourceLocation location = SourceLocation());


void Instrumentation::insert(
    CompoundStmt *c, const Stmt *before, ASTContext &ast) {

  vector<Stmt*> newChildren;
  vector<Stmt*> toAdd = create(ast);
  bool inserted = false;

  for (StmtRange s = c->children(); s; s++) {
    if (*s == before) {
      for (vector<Stmt*>::iterator i = toAdd.begin(); i != toAdd.end(); i++)
        newChildren.push_back(*i);
      inserted = true;
    }

    newChildren.push_back(*s);
  }

  assert(inserted && "Didn't find the Stmt to insert before");

  if (newChildren.size() > 0)
    c->setStmts(ast, &newChildren[0], newChildren.size());
}


void Instrumentation::replace(CompoundStmt *c, Stmt *s,
    ASTContext &ast, size_t len) {

  vector<Stmt*> toAdd = create(ast);
  assert(toAdd.size() <= len);

  for (size_t i = toAdd.size(); i < len; i++)
    toAdd.push_back(new (ast) NullStmt(Stmt::EmptyShell()));

  bool replacing = false;
  size_t i = 0;
  for (StmtRange children = c->children(); children; children++) {
    if (*children == s) {
      replacing = true;
      *children = toAdd[i++];
    } else if (replacing) {
      if (i == len) break;
      *children = toAdd[i++];
    }
  }
}

void Instrumentation::append(CompoundStmt *c, ASTContext &ast) {

  vector<Stmt*> newChildren;
  vector<Stmt*> toAdd = create(ast);

  for (StmtRange s = c->children(); s; s++) newChildren.push_back(*s);
  for (vector<Stmt*>::iterator i = toAdd.begin(); i != toAdd.end(); i++)
    newChildren.push_back(*i);

  c->setStmts(ast, &newChildren[0], newChildren.size());
}


pair<Expr*, vector<Stmt*> > Instrumentation::makeLValue(
    Expr *e, const string& name, DeclContext *dc, ASTContext &ast,
    SourceLocation location) {

  pair<Expr*, vector<Stmt*> > result;
  if (e->isLValue()) return result;

  // Create a temporary variable to store the result of 'e'.
  QualType t = e->getType();
  IdentifierInfo& id = ast.Idents.get("__tesla_tmp_" + name);

  VarDecl *decl = VarDecl::Create(
      ast, dc, location, &id, t, ast.CreateTypeSourceInfo(t),
      SC_None, SC_None);

  // Create a reference to this stored variable.
  result.first = new (ast) DeclRefExpr(decl, t, VK_LValue, location);

  // Declare the variable.
  dc->addDecl(decl);
  result.second.push_back(
      new (ast) DeclStmt(DeclGroupRef(decl), location, location));

  // Assign to the variable.
  result.second.push_back(
      new (ast) BinaryOperator(
        result.first, e, BO_Assign, t, VK_LValue, OK_Ordinary, location));

  return result;
}


string Instrumentation::typeIdentifier(const QualType t) const {
  string name = t.getAsString();

  size_t i;
  while ((i = name.find(' ')) != string::npos) name.replace(i, 1, "_");

  return name;
}

string Instrumentation::typeIdentifier(const Type *t) const {
  return typeIdentifier(QualType(t, Qualifiers()));
}


const string Instrumentation::PREFIX = "__tesla_event_";

string Instrumentation::eventHandlerName(const string& suffix) const {
  return PREFIX + suffix;
}



TeslaAssertion::TeslaAssertion(Expr *e, CompoundStmt *cs, FunctionDecl *f,
    int assertCount, Diagnostic& diag)
  : fnName(f->getName()), assertCount(assertCount), parent(cs),
    marker(dyn_cast<CallExpr>(e)), assertion(NULL)
{

  // Filter out anything that isn't the magic marker we're looking for
  // (call to __tesla_start_of_assertion()).
  if (marker == NULL) return;

  DeclRefExpr *dre =
    dyn_cast<DeclRefExpr>(marker->getCallee()->IgnoreParenCasts());
  if (dre == NULL) return;

  if (dre->getDecl()->getName() != "__tesla_start_of_assertion") return;


  // Find the block which immediately follows the marker.
  bool teslaBlockComesNext = false;

  for (StmtRange children = cs->children(); children; children++) {
    if (*children == e) teslaBlockComesNext = true;
    else if (teslaBlockComesNext) {
      // This should be the assertion block (a CompoundStmt).
      teslaBlockComesNext = false;
      this->assertion = dyn_cast<CompoundStmt>(*children);
    }
  }

  // Make sure that we did, in fact, find what we were looking for.
  if (assertion == NULL) {
    int id = diag.getCustomDiagID(
      Diagnostic::Error,
      "Expected Tesla assertion block at top level of compound statment");

    diag.Report(id) << e->getSourceRange();
    return;
  }

  searchForVariables(assertion);
}


void TeslaAssertion::searchForVariables(Stmt *s) {
  // Ignore callee function names; we're only interesting in fixing variables.
  if (CallExpr *call = dyn_cast<CallExpr>(s)) {
    typedef CallExpr::arg_iterator ArgIterator;
    for (ArgIterator i = call->arg_begin(); i != call->arg_end(); ++i)
      searchForVariables(*i);

  } else if (s->children()) {
    for (StmtRange child = s->children(); child; child++) {
      searchForVariables(*child);
    }

  } else if (DeclRefExpr *dre = dyn_cast<DeclRefExpr>(s)) {
    references.push_back(dre);
  }
}


vector<Stmt*> TeslaAssertion::create(ASTContext &ast) {
  // What shall we call our event handler?
  stringstream suffix;
  suffix << "assertion_";
  suffix << fnName;
  suffix << "_";
  suffix << assertCount;
  string handlerName = eventHandlerName(suffix.str());

  vector<Stmt*> statements;
  statements.push_back(call(handlerName, ast.VoidTy, references, ast));

  return statements;
}


FunctionEntry::FunctionEntry(FunctionDecl *function, QualType t)
  : f(function), teslaDataType(t)
{
  assert(!t.isNull() && "NULL type for 'struct __tesla_data'");
  assert(function->hasBody());
  assert(isa<CompoundStmt>(function->getBody()));

  name = function->getName();

  CompoundStmt *body = dyn_cast<CompoundStmt>(function->getBody());
  location = body->getLocStart();
}

vector<Stmt*> FunctionEntry::create(ASTContext &ast) {
  IdentifierInfo& dataName = ast.Idents.get("__tesla_data");
  vector<Stmt*> statements;

  VarDecl *data = VarDecl::Create(
      ast, f, location, &dataName,
      teslaDataType, ast.CreateTypeSourceInfo(teslaDataType),
      SC_None, SC_None
  );
  f->addDecl(data);

  statements.push_back(
      new (ast) DeclStmt(DeclGroupRef(data), location, location));


  vector<Expr*> parameters;
  parameters.push_back(
      addressOf(
        new (ast) DeclRefExpr(data, data->getType(), VK_RValue, location),
        ast, location));

  for (FunctionDecl::param_iterator i = f->param_begin();
       i != f->param_end(); i++) {
    SourceLocation loc = (*i)->getSourceRange().getBegin();
    parameters.push_back(
        new (ast) DeclRefExpr(*i, (*i)->getType(), VK_RValue, loc));
  }

  statements.push_back(call(eventHandlerName("function_prologue_" + name),
        ast.VoidTy, parameters, ast));

  return statements;
}


FunctionReturn::FunctionReturn(FunctionDecl *function, ReturnStmt *r)
  : f(function), r(r)
{
  assert(function->hasBody());
  assert(isa<CompoundStmt>(function->getBody()));

  name = function->getName();

  CompoundStmt *body = dyn_cast<CompoundStmt>(function->getBody());
  location = body->getLocEnd();
}

vector<Stmt*> FunctionReturn::create(ASTContext &ast) {
  // Find the local __tesla_data.
  DeclContextLookupResult lookupResult =
          f->lookup(DeclarationName(&ast.Idents.get("__tesla_data")));

  assert(isa<VarDecl>(*lookupResult.first));
  VarDecl *td = dyn_cast<VarDecl>(*lookupResult.first);

  QualType returnType = ast.VoidTy;
  vector<Expr*> parameters;
  parameters.push_back(
      addressOf(
        new (ast) DeclRefExpr(td, td->getType(), VK_RValue, SourceLocation()),
        ast, SourceLocation()));

  vector<Stmt*> statements;
  if ((r != NULL) and (r->getRetValue() != NULL)) {
    Expr *retval = r->getRetValue();
    returnType = retval->getType();

    pair<Expr*, vector<Stmt*> > lvalue = makeLValue(retval, "retval", f, ast);
    Expr *retLValue = lvalue.first;
    vector<Stmt*>& toAdd = lvalue.second;

    parameters.push_back(retLValue);
    r->setRetValue(retLValue);

    for (vector<Stmt*>::iterator i = toAdd.begin(); i != toAdd.end(); i++)
      statements.push_back(*i);
  }

  statements.push_back(
      call(eventHandlerName("function_return_" + name), returnType,
           parameters, ast));

  return statements;
}


FieldAssignment::FieldAssignment(MemberExpr *lhs, Expr *rhs, DeclContext *dc)
    : lhs(lhs), rhs(rhs), dc(dc) {
  assert(isa<FieldDecl>(lhs->getMemberDecl()));
  this->field = dyn_cast<FieldDecl>(lhs->getMemberDecl());

  this->structType = lhs->getBase()->getType();
  if (structType->isPointerType()) structType = structType->getPointeeType();
}



vector<Stmt*> FieldAssignment::create(ASTContext &ast) {
  vector<Stmt*> statements;

  // This is where we pretend the call was located.
  SourceLocation loc = lhs->getLocStart();

  // Ensure that we don't double-evaluate rhs.
  pair<Expr*, vector<Stmt*> > lvalue = makeLValue(rhs, "assign", dc, ast);
  vector<Stmt*>& init = lvalue.second;
  for (vector<Stmt*>::iterator i = init.begin(); i != init.end(); i++)
    statements.push_back(*i);

  // Get a pointer to the struct.
  Expr *base = lhs->getBase();
  if (!base->getType()->isPointerType()) base = addressOf(base, ast, loc);

  // Arguments: the base and the new value being assigned.
  vector<Expr*> arguments;
  arguments.push_back(base);
  arguments.push_back(lvalue.first);

  // The name of the event handler depends on the type and field names.
  string typeName = typeIdentifier(structType.getTypePtr());
  string fieldName = lhs->getMemberDecl()->getName();

  string name = eventHandlerName("field_assign_" + typeName + "_" + fieldName);

  // Call the event handler!
  statements.push_back(call(name, ast.VoidTy, arguments, ast));
  return statements;
}


Expr* cast(Expr *from, QualType to, ASTContext &ast) {
  // A cast from foo* to void* is a BitCast.
  CastKind castKind = CK_BitCast;

  // If casting from an arithmetic type, modify the CastKind accordingly and
  // remove all existing implicit casts.
  if (from->getType()->isArithmeticType()) {
    castKind = CK_IntegralToPointer;
    from = from->IgnoreParenImpCasts();
  }

  return CStyleCastExpr::Create(ast, to, VK_RValue, castKind, from, NULL,
      ast.getTrivialTypeSourceInfo(to), SourceLocation(), SourceLocation());
}

Expr* addressOf(Expr *e, ASTContext &ast, SourceLocation loc) {
  return new (ast) UnaryOperator(e, UO_AddrOf,
      ast.getPointerType(e->getType()), VK_RValue, OK_Ordinary, loc);
}

FunctionDecl *declareFn(const string& name, QualType returnType,
    vector<QualType>& argTypes, ASTContext &ast) {

  // The function doesn't throw exceptions, etc.
  FunctionProtoType::ExtProtoInfo extraInfo;

  QualType fnType = ast.getFunctionType(
      ast.VoidTy, &argTypes[0], argTypes.size(), extraInfo);

  FunctionDecl *fn = FunctionDecl::Create(ast, ast.getTranslationUnitDecl(),
     SourceLocation(), DeclarationName(&ast.Idents.get(name)), fnType, NULL,
     SC_Extern, SC_None);

  // Add the created declaration to the translation unit.
  ast.getTranslationUnitDecl()->addDecl(fn);

  return fn;
}


Expr *call(string name, QualType returnType, vector<Expr*>& params,
    ASTContext& ast, SourceLocation location) {

  vector<QualType> argTypes;
  for (vector<Expr*>::const_iterator i = params.begin(); i != params.end(); i++)
    argTypes.push_back((*i)->getType());

  FunctionDecl *fn = declareFn(name, returnType, argTypes, ast);

  Expr *fnPointer = new (ast) ImplicitCastExpr(
        ImplicitCastExpr::OnStack, ast.getPointerType(fn->getType()),
        CK_FunctionToPointerDecay,
        new (ast) DeclRefExpr(fn, fn->getType(), VK_RValue, location),
        VK_RValue);

  Expr** parameters = NULL;
  if (params.size() > 0) parameters = &params[0];

  return new (ast) CallExpr(ast, fnPointer, parameters, params.size(),
      ast.VoidTy, VK_RValue, location);
}
