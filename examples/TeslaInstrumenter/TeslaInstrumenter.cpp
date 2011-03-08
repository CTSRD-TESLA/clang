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

#define PREASSIGN_CHECKER_PREFIX "_check_store_"


/// Some instrumentation code.
///
/// For instance, before assigning to a variable, an Instrumentation instance
/// might call out to a function which says "yes, this is ok."
class Instrumentation {
  public:
    /// Creates the actual instrumentation code.
    virtual Stmt* create(ASTContext &ast) = 0;

    void insertBefore(CompoundStmt *c, Stmt *before, ASTContext &ast) {
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
    Expr *expression;
    QualType structType;
    FieldDecl *field;
    Expr *newValue;

  public:
    AssignHook(Expr *e = NULL, QualType structType = QualType(),
             FieldDecl *f = NULL);

    void setNewValue(Expr *e) { newValue = e; }

    inline bool isValid() {
      return
        (expression != NULL) and (field != NULL) and (newValue != NULL)
        and !structType.isNull();
    }

    virtual Stmt* create(ASTContext &ast);
};



/// Instruments assignments to tag fields with TESLA assertions.
class TeslaInstrumenter : public ASTConsumer {
private:
  QualType teslaDataType;
  set<const Type*> toInstrument;

  bool needToInstrument(const Type* t) const {
    return (toInstrument.find(t) != toInstrument.end());
  }

  bool needToInstrument(const QualType t) const {
    return needToInstrument(t.getTypePtr());
  }

public:
  void Visit(DeclContext *dc, ASTContext &ast);
  void Visit(Decl *d, DeclContext *context, ASTContext &ast);
  void Visit(Stmt *s, const CompoundStmt *cs, DeclContext* context,
      ASTContext &ast);
  void Visit(const Expr *e, const CompoundStmt *c, const Stmt *s,
      DeclContext* dc, ASTContext &ast);

  /// Adds a 'struct tesla_data' declaration to a CompoundStmt.
  void addTeslaDeclaration(
      CompoundStmt *c, DeclContext *dc, ASTContext &ast);

  AssignHook buildAssignHook(Expr *e);


  // ASTConsumer implementation.

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



// ********* Implementation (still in the anonymous namespace). ********

void TeslaInstrumenter::HandleTopLevelDecl(DeclGroupRef d) {
  for (DeclGroupRef::iterator i = d.begin(); i != d.end(); i++) {
    DeclContext *dc = dyn_cast<DeclContext>(*i);
    Visit(*i, dc, (*i)->getASTContext());
  }
}

void TeslaInstrumenter::HandleTagDeclDefinition(TagDecl *tag) {
  if (tag->getAttr<TeslaAttr>())
    toInstrument.insert(tag->getTypeForDecl());

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
  if (FunctionDecl *f = dyn_cast<FunctionDecl>(d))
    if (!f->isThisDeclarationADefinition())
      return;

  if (DeclContext *dc = dyn_cast<DeclContext>(d)) {
    Visit(dc, ast);
    context = dc;
  }

  if (d->hasBody()) {
    assert(isa<CompoundStmt>(d->getBody()));
    Visit(d->getBody(), dyn_cast<CompoundStmt>(d->getBody()), context, ast);
  }
}

void TeslaInstrumenter::Visit(
    Stmt *s, const CompoundStmt *cs, DeclContext* dc, ASTContext &ast) {

  if (s == NULL) return;

  if (Expr *e = dyn_cast<Expr>(s)) {
    Visit(e, cs, s, dc, ast);
    return;
  }

  if (CompoundStmt *c = dyn_cast<CompoundStmt>(s)) {
    cs = c;
    /*
    if (!teslaDataType.isNull())
      addTeslaDeclaration(c, dc, ast);
    */

  } else if (DeclStmt *ds = dyn_cast<DeclStmt>(s)) {
    typedef DeclStmt::decl_iterator Iterator;
    for (Iterator i = ds->decl_begin(); i != ds->decl_end(); i++) {
      if (VarDecl *vd = dyn_cast<VarDecl>(*i)) {
        const Expr *e = vd->getAnyInitializer();
        if (e != NULL) Visit(e, cs, s, dc, ast);
      }
    }
  }

  for (StmtRange child = s->children(); child; child++)
    Visit(*child, cs, dc, ast);
}


void TeslaInstrumenter::Visit(const Expr *e,
    const CompoundStmt *cs, const Stmt *s, DeclContext* dc, ASTContext &ast) {

  if (const BinaryOperator *o = dyn_cast<BinaryOperator>(e)) {
    Expr *lhs = o->getLHS();
    Expr *rhs = o->getRHS();

    Visit(lhs, cs, s, dc, ast);
    Visit(rhs, cs, s, dc, ast);

    if (o->isAssignmentOp()) {
      AssignHook base = buildAssignHook(lhs);
      base.setNewValue(rhs);

      if (base.isValid()) {
        Diagnostic &D = ast.getDiagnostics();
        unsigned diagID = D.getCustomDiagID(
          Diagnostic::Warning, "Assignment needs TESLA instrumentation");
        D.Report(e->getLocStart(), diagID)
          << e->getSourceRange();

        llvm::errs() << "call: ";
        base.create(ast)->dumpPretty(ast);
        llvm::errs() << " before '";
        s->dumpPretty(ast);
        llvm::errs() << "'\n\n";
      }
    }
  }

  for (ConstStmtRange child = e->children(); child; child++) {
    assert(isa<Expr>(*child) && "Non-Expr child of Expr");
    Visit(dyn_cast<Expr>(*child), cs, s, dc, ast);
  }
}



AssignHook TeslaInstrumenter::buildAssignHook(Expr *e) {
  MemberExpr *me = dyn_cast<MemberExpr>(e);
  if (me == NULL) return AssignHook();

  // Do we even want to instrument this type?
  QualType baseType = me->getBase()->getType();
  while (baseType->isPointerType()) baseType = baseType->getPointeeType();

  const RecordType *rt = baseType->getAsStructureType();
  assert(rt != NULL && "MemberExpr base should be a structure");
  baseType = rt->desugar();

  if (!needToInstrument(baseType)) return AssignHook();

  // Which of the structure's fields are we accessing?
  const RecordDecl *decl = rt->getDecl();
  FieldDecl *field = NULL;

  string memberName = me->getMemberDecl()->getName();
  typedef RecordDecl::field_iterator Iterator;
  for (Iterator i = decl->field_begin(); i != decl->field_end(); i++)
    if (i->getName() == memberName) {
      field = *i;
      break;
    }

    return AssignHook(me->getBase(), baseType, field);
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



AssignHook::AssignHook(Expr *e, QualType structType, FieldDecl *f)
        : expression(e),
          structType(structType),
          field(f) {
}

Stmt* AssignHook::create(ASTContext &ast) {
  assert(isValid());

  if (!expression->getType()->isPointerType()) {
    expression = new (ast) UnaryOperator(
        expression, UO_AddrOf, structType, VK_RValue, OK_Ordinary,
        expression->getLocStart());
  }

  llvm::errs()
    << PREASSIGN_CHECKER_PREFIX
    << QualType::getAsString(
        field->getParent()->getTypeForDecl(),
        Qualifiers()).substr(7)
    << "(";
  expression->dumpPretty(ast);
  llvm::errs()
    << ", " << field->getFieldIndex()
    << ", ";

  if (newValue) newValue->dumpPretty(ast);
  else llvm::errs() << "<no expression yet>";

  llvm::errs()
    << ")";

  return new (ast) NullStmt(expression->getLocStart());
}

}
