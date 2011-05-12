/*-
 * Copyright (c) 2011 Jonathan Anderson, Steven J. Murdoch
 * All rights reserved.
 *
 * This software was developed by SRI International and the University of
 * Cambridge Computer Laboratory under DARPA/AFRL contract (FA8750-10-C-0237)
 * ("CTSRD"), as part of the DARPA CRASH research programme.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sstream>

#include <llvm/ADT/StringSwitch.h>

#include "Instrumentation.h"

using namespace clang;
using namespace std;

using llvm::StringSwitch;


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

/// Insert create(ast) (a vector of Stmnt*) into the children of c 
/// before Stmt before. Returns create(ast) added.
vector<Stmt*> Instrumentation::insert(
    CompoundStmt *c, const Stmt *before, ASTContext &ast) {

  vector<Stmt*> newChildren;
  vector<Stmt*> toAdd = create(ast);
  bool inserted = false;

  for (StmtRange s = c->children(); s; s++) {
    if (*s == before) {
      // XXX: This will insert toAdd multiple times, should it?
      for (vector<Stmt*>::iterator i = toAdd.begin(); i != toAdd.end(); i++)
        newChildren.push_back(*i);
      inserted = true;
    }

    newChildren.push_back(*s);
  }

  assert(inserted && "Didn't find the Stmt to insert before");

  if (newChildren.size() > 0)
    c->setStmts(ast, &newChildren[0], newChildren.size());

  return toAdd;
}

// Replaces s and the len-1 following statements within the children of c with
// create(ast). If create(ast) has fewer statements than len, NullStmts are
// added after to make them of equal length. create(ast) must have at least len
// elements. Returns the new statements inserted.
vector<Stmt*> Instrumentation::replace(CompoundStmt *c, Stmt *s,
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

  return toAdd;
}

// Appends create(ast) after the last child of c. Returns create(ast)
vector<Stmt*> Instrumentation::append(CompoundStmt *c, ASTContext &ast) {

  vector<Stmt*> newChildren;
  vector<Stmt*> toAdd = create(ast);

  for (StmtRange s = c->children(); s; s++) newChildren.push_back(*s);
  for (vector<Stmt*>::iterator i = toAdd.begin(); i != toAdd.end(); i++)
    newChildren.push_back(*i);

  c->setStmts(ast, &newChildren[0], newChildren.size());

  return toAdd;
}

/// Create a temporary variable __tesla_tmp_<name>. Returns a pair
/// where the first expression is a reference to the variable
/// and the second statements assigns e to this variable.
pair<Expr*, vector<Stmt*> > Instrumentation::makeLValue(
    Expr *e, const string& name, DeclContext *dc, ASTContext &ast,
    SourceLocation location) {

  pair<Expr*, vector<Stmt*> > result;
  // XXX: Is returning an empty pair correct?
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


/// Returns name of t, with spaces replaced by "_"
string Instrumentation::typeIdentifier(const QualType t) const {
  string name = t.getAsString();

  size_t i;
  while ((i = name.find(' ')) != string::npos) name.replace(i, 1, "_");

  return name;
}

/// Returns name of t
string Instrumentation::typeIdentifier(const Type *t) const {
  return typeIdentifier(QualType(t, Qualifiers()));
}


const string Instrumentation::PREFIX = "__tesla_event_";

/// Returns __tesla_event_<suffix>
string Instrumentation::eventHandlerName(const string& suffix) const {
  return PREFIX + suffix;
}



TeslaAssertion::TeslaAssertion(Expr *e, CompoundStmt *cs, FunctionDecl *f,
    int assertCount, Diagnostic& diag)
  : diag(diag), f(f), assertCount(assertCount), parent(cs),
    marker(dyn_cast<CallExpr>(e)), assertion(NULL),
    scopeBegin(NULL), scopeEnd(NULL)
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

  stringstream suffix;
  suffix << "assertion_";
  suffix << f->getName().str();
  suffix << "_";
  suffix << assertCount;
  handlerName = eventHandlerName(suffix.str());

  // We expect the 'marker' to have three arguments:
  //   storage      a call to __tesla_storage_(global|perthread)
  //   scope_b      a call to __tesla_enter(a function)
  //   scope_e      a call to __tesla_leave(a function)
  const static size_t ARGS = 3;
  assert(marker->getNumArgs() == ARGS);

  FunctionDecl* callees[ARGS];
  for (size_t i = 0; i < ARGS; i++) {
    Expr *arg = marker->getArg(i)->IgnoreParenCasts();
    CallExpr *call = dyn_cast<CallExpr>(arg);

    if (call == NULL) {
      int id = diag.getCustomDiagID(Diagnostic::Error,
          "Tesla scope is not a function call");
      diag.Report(arg->getLocStart(), id) << marker->getSourceRange();
      return;
    } else if ((callees[i] = call->getDirectCallee()) == NULL) {
      int id = diag.getCustomDiagID(Diagnostic::Error,
          "Tesla scope is not a function");
      diag.Report(dre->getLocStart(), id) << marker->getSourceRange();
      return;
    }

    // Output spec and instrumentation needed to identify scope entry and exit
    searchForReferences(call);
  }

  storage = StringSwitch<StorageClass>(callees[0]->getName())
    .Case("__tesla_storage_global", GLOBAL)
    .Case("__tesla_storage_perthread", PER_THREAD)
    .Default(UNKNOWN);

  scopeBegin = callees[1];
  scopeEnd = callees[2];

  // Output spec and instrumentation needed to evaluate assertion
  searchForReferences(assertion);
}

TeslaAssertion::TeslaAssertion(const TeslaAssertion& orig)
  : Instrumentation(),
    diag(orig.diag),
    f(orig.f),
    handlerName(orig.handlerName),
    assertCount(orig.assertCount),
    parent(orig.parent),
    marker(orig.marker),
    assertion(orig.assertion),
    storage(orig.storage),
    scopeBegin(orig.scopeBegin),
    scopeEnd(orig.scopeEnd),
    variableRefs(orig.variableRefs),
    variables(orig.variables),
    functions(orig.functions)
{
}

TeslaAssertion& TeslaAssertion::operator= (const TeslaAssertion& rhs) {
  *this = rhs;
  return *this;
}


/// Sets variables, variableRefs, functions[fn] = parameters based
/// on variables and functions referenced in the assertion
void TeslaAssertion::searchForReferences(Stmt *s) {
  if (CallExpr *call = dyn_cast<CallExpr>(s)) {
    FunctionDecl *fn = call->getDirectCallee();
    if (fn == NULL) {
      int id = diag.getCustomDiagID(Diagnostic::Error,
          "Indirect function call within Tesla assertion");
      diag.Report(call->getLocStart(), id) << s->getSourceRange();
      return;
    }

    // In tesla.h, now, eventually, previously, invoked, returned,
    // assigned, dont_care etc. are prepended with __tesla_  
    bool isRealFunction = !fn->getName().startswith("__tesla");
    vector<Expr*> parameters;

    typedef CallExpr::arg_iterator ArgIterator;
    for (ArgIterator i = call->arg_begin(); i != call->arg_end(); ++i) {
      if (isRealFunction) parameters.push_back(*i);
      searchForReferences(*i);
    }

    if (isRealFunction) functions[fn] = parameters;

  } else if (s->children()) {
    for (StmtRange child = s->children(); child; child++) {
      searchForReferences(*child);
    }

  } else if (DeclRefExpr *dre = dyn_cast<DeclRefExpr>(s)) {
    Decl *d = dre->getDecl();
    if (variables.find(d) == variables.end()) {
      variables.insert(d);
      variableRefs.push_back(dre);
    }
  }
}


vector<Stmt*> TeslaAssertion::create(ASTContext &ast) {
  return vector<Stmt*>(1, call(handlerName, ast.VoidTy, variableRefs, ast));
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


const ValueDecl* TeslaAssertion::getVariable(size_t i) const {
  assert(isa<DeclRefExpr>(variableRefs[i]->IgnoreParenCasts()));
  DeclRefExpr *d = dyn_cast<DeclRefExpr>(variableRefs[i]->IgnoreParenCasts());

  assert(isa<ValueDecl>(d->getDecl()));
  return dyn_cast<ValueDecl>(d->getDecl());
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
