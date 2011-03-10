/// \file TeslaInstrumenter and TeslaAction

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

using namespace clang;
using namespace std;

namespace {

/// Explicitly cast an expression to a type (probably only works for casting
/// things to pointer types).
Expr* cast(Expr *from, QualType to, ASTContext &ast);

/// Take the address of an expression result.
Expr* addressOf(Expr*, ASTContext&, SourceLocation loc = SourceLocation());

/// Declare a function.
FunctionDecl *declareFn(const string& name, QualType returnType,
    const vector<QualType>& argTypes, ASTContext &ast);


/// Some instrumentation code.
///
/// For instance, before assigning to a variable, an Instrumentation instance
/// might call out to a function which says "yes, this is ok."
class Instrumentation {
  public:
    /// Creates the actual instrumentation code.
    virtual Stmt* create(ASTContext &ast) = 0;

    void insertBefore(CompoundStmt *c, const Stmt *before, ASTContext &ast) {
      vector<Stmt*> newChildren;
      for (StmtRange s = c->children(); s; s++) {
        if (*s == before) newChildren.push_back(create(ast));
        newChildren.push_back(*s);
      }

      c->setStmts(ast, &newChildren[0], newChildren.size());
    }

    void insert(CompoundStmt *c, ASTContext &ast) {
      insertBefore(c, *c->children(), ast);
    }
};


/// A hook to call out to an external checker.
class AssignHook : public Instrumentation {
  private:
    const static std::string PREFIX;

    MemberExpr *lhs;
    Expr *rhs;
    QualType structType;
    FieldDecl *field;

    /// The name of the function which we will call to check this assignment.
    std::string checkerName() const;

  public:
    AssignHook(MemberExpr *lhs, Expr *rhs);
    virtual Stmt* create(ASTContext &ast);
};


/// Instruments assignments to tag fields with TESLA assertions.
class TeslaInstrumenter : public ASTConsumer {
private:
  QualType teslaDataType;
  set<const Type*> toInstrument;

  Diagnostic *diag;
  unsigned int teslaWarningId;

  bool needToInstrument(const Type* t) const {
    return (toInstrument.find(t) != toInstrument.end());
  }

  bool needToInstrument(const QualType t) const {
    return needToInstrument(t.getTypePtr());
  }

  DiagnosticBuilder warnAddingInstrumentation(SourceLocation) const;


public:
  void Visit(DeclContext *dc, ASTContext &ast);
  void Visit(Decl *d, DeclContext *context, ASTContext &ast);
  void Visit(CompoundStmt *cs, DeclContext* context, ASTContext &ast);
  void Visit(Stmt *s, CompoundStmt *cs, DeclContext* context,
      ASTContext &ast);
  void Visit(Expr *e, Stmt *s, CompoundStmt *c,
      DeclContext* dc, ASTContext &ast);
  void Visit(
      BinaryOperator *o, Stmt *s, CompoundStmt *cs, ASTContext &ast);

  /// Adds a 'struct tesla_data' declaration to a CompoundStmt.
  void addTeslaDeclaration(
      CompoundStmt *c, DeclContext *dc, ASTContext &ast);


  // ASTConsumer implementation.

  virtual void Initialize(ASTContext& ast);

  /// Make note if a tag has been tagged with __tesla or the like.
  virtual void HandleTagDeclDefinition(TagDecl *tag);

  /// Recurse down through a declaration of a variable, function, etc.
  virtual void HandleTopLevelDecl(DeclGroupRef d);
};


class TeslaAction : public PluginASTAction {
protected:
  ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) {
    return new TeslaInstrumenter();
  }

  bool ParseArgs(const CompilerInstance &CI, const vector<string>& args) {
    return true;
  }

  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Help for TeslaInstrumenter plugin goes here\n";
  }
};

static FrontendPluginRegistry::Add<TeslaAction>
X("tesla", "Add TESLA instrumentation");



// ********* TeslaInstrumenter (still in the anonymous namespace). ********

void TeslaInstrumenter::Initialize(ASTContext& ast) {
  diag = &ast.getDiagnostics();
  teslaWarningId = diag->getCustomDiagID(
      Diagnostic::Warning, "Adding TESLA instrumentation");
}

void TeslaInstrumenter::HandleTopLevelDecl(DeclGroupRef d) {
  for (DeclGroupRef::iterator i = d.begin(); i != d.end(); i++) {
    DeclContext *dc = dyn_cast<DeclContext>(*i);
    Visit(*i, dc, (*i)->getASTContext());
  }
}

void TeslaInstrumenter::HandleTagDeclDefinition(TagDecl *tag) {
  if (tag->getAttr<TeslaAttr>()) {
    toInstrument.insert(tag->getTypeForDecl());
  }

  else if (tag->getName() == "tesla_data")
    teslaDataType = tag->getTypeForDecl()->getCanonicalTypeInternal();
}



void TeslaInstrumenter::Visit(DeclContext *dc, ASTContext &ast) {
  typedef DeclContext::decl_iterator Iterator;
  for (Iterator i = dc->decls_begin(); i != dc->decls_end(); i++) {
    Visit(*i, dc, ast);
  }
}


void TeslaInstrumenter::Visit(Decl *d, DeclContext *context, ASTContext &ast) {
  // We're not interested in function declarations, only definitions.
  if (FunctionDecl *f = dyn_cast<FunctionDecl>(d)) {
    if (!f->isThisDeclarationADefinition())
      return;
  }

  if (DeclContext *dc = dyn_cast<DeclContext>(d)) {
    Visit(dc, ast);
    context = dc;
  }

  if (d->hasBody()) {
    assert(isa<CompoundStmt>(d->getBody()));
    Visit(dyn_cast<CompoundStmt>(d->getBody()), context, ast);
  }
}

void TeslaInstrumenter::Visit(
    CompoundStmt *cs, DeclContext* dc, ASTContext &ast) {

  assert(cs != NULL);

  for (StmtRange child = cs->children(); child; child++) {
    // It's perfectly legitimate to have a null child (think an IfStmt with
    // no 'else' clause), but dyn_cast() will choke on it.
    if (*child == NULL) continue;

    if (Expr *e = dyn_cast<Expr>(*child)) Visit(e, e, cs, dc, ast);
    else Visit(*child, cs, dc, ast);
  }
}

void TeslaInstrumenter::Visit(
    Stmt *s, CompoundStmt *cs, DeclContext* dc, ASTContext &ast) {

  assert(s != NULL);

  if (CompoundStmt *c = dyn_cast<CompoundStmt>(s)) Visit(c, dc, ast);
  else
    for (StmtRange child = s->children(); child; child++) {
      // It's perfectly legitimate to have a null child (think an IfStmt with
      // no 'else' clause), but dyn_cast() will choke on it.
      if (*child == NULL) continue;

      if (Expr *e = dyn_cast<Expr>(*child)) Visit(e, s, cs, dc, ast);
      else Visit(*child, cs, dc, ast);
    }
}

void TeslaInstrumenter::Visit(Expr *e,
    Stmt *s, CompoundStmt *cs, DeclContext* dc, ASTContext &ast) {

  assert(e != NULL);

  if (BinaryOperator *o = dyn_cast<BinaryOperator>(e))
    Visit(o, s, cs, ast);

  for (StmtRange child = e->children(); child; child++) {
    assert(isa<Expr>(*child) && "Non-Expr child of Expr");
    Visit(dyn_cast<Expr>(*child), s, cs, dc, ast);
  }
}

void TeslaInstrumenter::Visit(
    BinaryOperator *o, Stmt *s, CompoundStmt *cs, ASTContext &ast) {

  if (!o->isAssignmentOp()) return;

  // We only care about assignments to structure fields.
  MemberExpr *lhs = dyn_cast<MemberExpr>(o->getLHS());
  if (lhs == NULL) return;

  // Do we want to instrument this type?
  QualType baseType = lhs->getBase()->getType();
  if (baseType->isPointerType()) baseType = baseType->getPointeeType();
  if (!needToInstrument(baseType)) return;

  Expr *rhs = o->getRHS();
  switch (o->getOpcode()) {
    case BO_Assign:
    case BO_MulAssign:
    case BO_DivAssign:
    case BO_RemAssign:
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_ShlAssign:
    case BO_ShrAssign:
    case BO_AndAssign:
    case BO_XorAssign:
    case BO_OrAssign:
      break;

    default:
      assert(false && "isBinaryInstruction() => non-assign opcode");
  }

  AssignHook hook(lhs, rhs);
  warnAddingInstrumentation(o->getLocStart()) << o->getSourceRange();
  hook.insertBefore(cs, s, ast);
}


void TeslaInstrumenter::addTeslaDeclaration(
    CompoundStmt *c, DeclContext *dc, ASTContext &ast) {

  VarDecl *teslaDecl = VarDecl::Create(
      ast, dc, c->getLocStart(), &ast.Idents.get("tesla_data"),
      teslaDataType, ast.CreateTypeSourceInfo(teslaDataType),
      SC_None, SC_None
  );

  dc->addDecl(teslaDecl);

  DeclStmt *stmt = new (ast) DeclStmt(
    DeclGroupRef(teslaDecl), c->getLocStart(), c->getLocEnd());

  vector<Stmt*> newChildren(1, stmt);
  for (StmtRange s = c->children(); s; s++) newChildren.push_back(*s);

  c->setStmts(ast, &newChildren[0], newChildren.size());
}


DiagnosticBuilder
TeslaInstrumenter::warnAddingInstrumentation(SourceLocation loc) const {
   return diag->Report(loc, teslaWarningId);
}



// ********* AssignHook (still in the anonymous namespace). ********

AssignHook::AssignHook(MemberExpr *lhs, Expr *rhs) : lhs(lhs), rhs(rhs) {
  assert(isa<FieldDecl>(lhs->getMemberDecl()));
  this->field = dyn_cast<FieldDecl>(lhs->getMemberDecl());

  this->structType = lhs->getBase()->getType();
  if (structType->isPointerType()) structType = structType->getPointeeType();
}



Stmt* AssignHook::create(ASTContext &ast) {
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


const string AssignHook::PREFIX = "__tesla_check_field_assign_";

string AssignHook::checkerName() const {
  string typeName =
    QualType::getAsString(structType.getTypePtr(), Qualifiers());

  // Replace all spaces with underscores (e.g. 'struct Foo' => 'struct_Foo')
  size_t i;
  while ((i = typeName.find(' ')) != string::npos) typeName.replace(i, 1, "_");

  return PREFIX + typeName;
}



// ********* Helper functions (still in the anonymous namespace). ********

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

}
