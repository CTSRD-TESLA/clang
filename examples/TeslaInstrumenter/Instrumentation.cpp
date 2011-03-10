
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
    const vector<QualType>& argTypes, ASTContext &ast);



void Instrumentation::insert(
    CompoundStmt *c, const Stmt *before, ASTContext &ast) {

  vector<Stmt*> newChildren;
  for (StmtRange s = c->children(); s; s++) {
    if (*s == before) newChildren.push_back(create(ast));
    newChildren.push_back(*s);
  }

  c->setStmts(ast, &newChildren[0], newChildren.size());
}




FieldAssignment::FieldAssignment(MemberExpr *lhs, Expr *rhs)
    : lhs(lhs), rhs(rhs) {
  assert(isa<FieldDecl>(lhs->getMemberDecl()));
  this->field = dyn_cast<FieldDecl>(lhs->getMemberDecl());

  this->structType = lhs->getBase()->getType();
  if (structType->isPointerType()) structType = structType->getPointeeType();
}



Stmt* FieldAssignment::create(ASTContext &ast) {
  // This is where we pretend the call was located.
  SourceLocation loc = lhs->getLocStart();

  // Get a pointer to the struct.
  Expr *base = lhs->getBase();
  if (!base->getType()->isPointerType()) base = addressOf(base, ast, loc);

  // Create a function declaration within the context of the whole translation
  // unit (this will be uniqued if necessary by the CG).
  vector<QualType> argTypes(1, base->getType());
  argTypes.push_back(ast.IntTy);
  argTypes.push_back(ast.VoidPtrTy);

  FunctionDecl *fn = declareFn(checkerName(), ast.VoidTy, argTypes, ast);

  // Construct the expression that we will use to call said function.
  Expr *fnExpr = new (ast) ImplicitCastExpr(
        ImplicitCastExpr::OnStack, ast.getPointerType(fn->getType()),
        CK_FunctionToPointerDecay, 
        new (ast) DeclRefExpr(fn, fn->getType(), VK_RValue, loc), VK_RValue);

  Expr* args[3] = {
    base,
    new (ast) IntegerLiteral(
        ast, ast.MakeIntValue(field->getFieldIndex(), argTypes[1]),
        argTypes[1], loc),
    cast(rhs, argTypes[2], ast)
  };

  return new (ast) CallExpr(ast, fnExpr, args, 3, ast.VoidTy, VK_RValue, loc);
}


const string FieldAssignment::PREFIX = "__tesla_check_field_assign_";

string FieldAssignment::checkerName() const {
  string typeName =
    QualType::getAsString(structType.getTypePtr(), Qualifiers());

  // Replace all spaces with underscores (e.g. 'struct Foo' => 'struct_Foo')
  size_t i;
  while ((i = typeName.find(' ')) != string::npos) typeName.replace(i, 1, "_");

  return PREFIX + typeName;
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
    const vector<QualType>& argTypes, ASTContext &ast) {

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
