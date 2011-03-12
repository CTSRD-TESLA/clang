
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

void Instrumentation::append(CompoundStmt *c, ASTContext &ast) {

  vector<Stmt*> newChildren;
  vector<Stmt*> toAdd = create(ast);

  for (StmtRange s = c->children(); s; s++) newChildren.push_back(*s);
  for (vector<Stmt*>::iterator i = toAdd.begin(); i != toAdd.end(); i++)
    newChildren.push_back(*i);

  c->setStmts(ast, &newChildren[0], newChildren.size());
}


const string Instrumentation::PREFIX = "__tesla_event_";

string Instrumentation::checkerName(const string& suffix) const {
  return PREFIX + suffix;
}


FunctionEntry::FunctionEntry(FunctionDecl *function, QualType t)
  : f(function), teslaDataType(t)
{
  assert(!t.isNull());
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
  for (FunctionDecl::param_iterator i = f->param_begin();
       i != f->param_end(); i++) {
    parameters.push_back(new (ast) DeclRefExpr(*i, (*i)->getType(), VK_RValue,
        (*i)->getSourceRange().getBegin()));
  }

  statements.push_back(call(checkerName("function_prologue_" + name),
        ast.VoidTy, parameters, ast));

  return statements;
}


FunctionReturn::FunctionReturn(FunctionDecl *function, Expr *retval)
  : f(function), retval(retval)
{
  assert(function->hasBody());
  assert(isa<CompoundStmt>(function->getBody()));

  name = function->getName();

  CompoundStmt *body = dyn_cast<CompoundStmt>(function->getBody());
  location = body->getLocEnd();
}

vector<Stmt*> FunctionReturn::create(ASTContext &ast) {
  /*
  IdentifierInfo& dataName = ast.Idents.get("__tesla_data");
  DeclContext::lookup_result result = f->lookup(DeclarationName(&dataName));
  */

  QualType returnType = ast.VoidTy;
  vector<Expr*> parameters;

  if (retval != NULL) {
    parameters.push_back(retval);
    returnType = retval->getType();
  }

  return vector<Stmt*>(1, call(checkerName("function_return_" + name),
      returnType, parameters, ast));
}


FieldAssignment::FieldAssignment(MemberExpr *lhs, Expr *rhs)
    : lhs(lhs), rhs(rhs) {
  assert(isa<FieldDecl>(lhs->getMemberDecl()));
  this->field = dyn_cast<FieldDecl>(lhs->getMemberDecl());

  this->structType = lhs->getBase()->getType();
  if (structType->isPointerType()) structType = structType->getPointeeType();
}



vector<Stmt*> FieldAssignment::create(ASTContext &ast) {
  // This is where we pretend the call was located.
  SourceLocation loc = lhs->getLocStart();

  // Get a pointer to the struct.
  Expr *base = lhs->getBase();
  if (!base->getType()->isPointerType()) base = addressOf(base, ast, loc);

  vector<Expr*> arguments;
  arguments.push_back(base);
  arguments.push_back(new (ast) IntegerLiteral(
        ast, ast.MakeIntValue(field->getFieldIndex(), ast.IntTy),
        ast.IntTy, loc));
  arguments.push_back(cast(rhs, ast.VoidPtrTy, ast));

  // Get the name of the type.
  string typeName =
    QualType::getAsString(structType.getTypePtr(), Qualifiers());

  // Replace all spaces with underscores (e.g. 'struct Foo' => 'struct_Foo')
  size_t i;
  while ((i = typeName.find(' ')) != string::npos) typeName.replace(i, 1, "_");

  return vector<Stmt*>(1, call(checkerName("struct_assign_" + typeName),
        ast.VoidTy, arguments, ast));
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
