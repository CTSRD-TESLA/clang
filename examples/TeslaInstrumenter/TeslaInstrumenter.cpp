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

  typedef Stmt::child_iterator StmtIterator;

  bool needToInstrument(const Type* t) const {
    return (toInstrument.find(t) != toInstrument.end());
  }

  bool needToInstrument(const QualType t) const {
    return needToInstrument(t.getTypePtr());
  }

public:
  class BaseInfo {
    private:
      Expr *expression;
      QualType structType;
      FieldDecl *field;

    public:
      BaseInfo(Expr *e = NULL, QualType structType = QualType(),
               FieldDecl *f = NULL)
        : expression(e),
          structType(structType),
          field(f) {
      }

      bool isValid() {
        return
          (expression != NULL) and !structType.isNull() and (field != NULL);
      }

      void buildCall(ASTContext &ast, Expr *newValue = NULL) {
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
      }
  };

  BaseInfo buildBase(Expr *e) {
    MemberExpr *me = dyn_cast<MemberExpr>(e);
    if (me == NULL) return BaseInfo();

    // Do we even want to instrument this type?
    QualType baseType = me->getBase()->getType();
    while (baseType->isPointerType()) baseType = baseType->getPointeeType();

    const RecordType *rt = baseType->getAsStructureType();
    assert(rt != NULL && "MemberExpr base should be a structure");
    baseType = rt->desugar();

    if (!needToInstrument(baseType)) return BaseInfo();

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

    return BaseInfo(me->getBase(), baseType, field);
  }

  /// Recursively visits an Expr.
  void VisitExpression(const Expr *e, DeclContext* dc, ASTContext &ast) {
    e = e->IgnoreParenCasts();

    if (const BinaryOperator *o = dyn_cast<BinaryOperator>(e)) {
      Expr *lhs = o->getLHS();
      Expr *rhs = o->getRHS();

      VisitExpression(lhs, dc, ast);
      VisitExpression(rhs, dc, ast);

      if (o->isAssignmentOp()) {
        BaseInfo base = buildBase(lhs);
        if (base.isValid()) {
          Diagnostic &D = ast.getDiagnostics();
          unsigned diagID = D.getCustomDiagID(
            Diagnostic::Warning, "Assignment needs TESLA instrumentation");
          D.Report(e->getLocStart(), diagID)
            << e->getSourceRange();

          llvm::errs() << "call: ";
          base.buildCall(ast, rhs);
          llvm::errs() << "\n\n";
        }
      }
#if 0
    } else if (DeclRefExpr *dre = dyn_cast<DeclRefExpr>(e)) {
      ValueDecl *d = dre->getDecl();
      /*
      llvm::errs()
        << "DeclRefExpr: '" << d->getName()
        << "' (of type '" << d->getType().getAsString()
        << "')\n";

      dre->dump();
        */

      QualType t = d->getType();
      while (t->isPointerType()) {
        t = t->getPointeeType();
       // ->getPointeeType();
      }

      /*
      if (needToInstrument(t)) {
        llvm::errs() << "AHA! We need to instrument this!\n";

        llvm::errs()
          << "call check_" << t.getAsString()
          << "(" << d->getName()
          << "\n";
      }
      */
#endif
    }
    else {
      return;
    }

    typedef Stmt::const_child_iterator Iterator;
    for (Iterator i = e->child_begin(); i != e->child_end(); i++) {
      const Expr *child = dyn_cast<Expr>(*i);
      assert(child && "Non-Expr child of Expr");

      VisitExpression(child, dc, ast);
    }
  }


  /// Adds a 'struct tesla_data' declaration to a CompoundStmt.
  void addTeslaDeclaration(
      CompoundStmt *c, DeclContext *dc, ASTContext &ast) {

    VarDecl *teslaDecl = VarDecl::Create(
        ast, dc, c->getLocStart(), &ast.Idents.get("tesla_data"),
        teslaDataType, ast.CreateTypeSourceInfo(teslaDataType),
        SC_None, SC_None
    );

    vector<Stmt*> newChildren;
    newChildren.push_back(new (ast) DeclStmt(
        DeclGroupRef(teslaDecl), c->getLocStart(), c->getLocEnd()));

    for (StmtIterator i = c->child_begin(); i != c->child_end(); i++)
      newChildren.push_back(*i);

    c->setStmts(ast, &newChildren[0], newChildren.size());

    llvm::errs() << "--------------------\n";
    llvm::errs() << "Added tesla_data:\n";
    llvm::errs() << "--------------------\n";
    c->dumpPretty(ast);
    llvm::errs() << "--------------------\n";
  }

  /// Recursively visits a Stmt.
  void VisitStatement(Stmt *s, DeclContext* dc, ASTContext &ast) {
    assert(s != NULL && "VisitStatement(NULL)");

    if (false) {
      llvm::errs()
        << "\n============== statement: =================\n"
        << s->getStmtClassName() << ": '";

      s->dumpPretty(ast);
      llvm::errs() << "'\n";
    }

    if (Expr *e = dyn_cast<Expr>(s)) {
      VisitExpression(e, dc, ast);
    } else if (DeclStmt *ds = dyn_cast<DeclStmt>(s)) {
      typedef DeclStmt::decl_iterator Iterator;
      for (Iterator i = ds->decl_begin(); i != ds->decl_end(); i++) {
        if (VarDecl *vd = dyn_cast<VarDecl>(*i)) {
          const Expr *e = vd->getAnyInitializer();
          if (e != NULL) VisitExpression(e, dc, ast);
        }
      }
    } else if (CompoundStmt *c = dyn_cast<CompoundStmt>(s)) {
//      addTeslaDeclaration(c, dc, ast);

      for (StmtIterator i = c->child_begin(); i != s->child_end(); i++)
        VisitStatement(*i, dc, ast);
    }
  }

  /// Recursively visits a Decl.
  void VisitDecl(Decl *d, DeclContext *context, ASTContext &ast) {
    // We're not interested in function declarations, only definitions.
    if (FunctionDecl *f = dyn_cast<FunctionDecl>(d)) {
      if (!f->isThisDeclarationADefinition()) return;
//      if (f->getName() != "helper") return;
    }

    if (DeclContext *dc = dyn_cast<DeclContext>(d)) {
      VisitDeclContext(dc, ast);
      context = dc;
    }

    if (d->hasBody())
      VisitStatement(d->getBody(), context, d->getASTContext());
  }

  /// Visits a DeclContext and all of its Decl children.
  void VisitDeclContext(DeclContext *dc, ASTContext &ast) {
    typedef DeclContext::decl_iterator Iterator;
    for (Iterator i = dc->decls_begin(); i != dc->decls_end(); i++) {
      VisitDecl(*i, dc, ast);
    }
  }



  // ASTConsumer implementation.

  /// Make note if a tag has been tagged with __tesla or the like.
  virtual void HandleTagDeclDefinition(TagDecl *tag) {
    if (tag->getAttr<TeslaAttr>()) {
      toInstrument.insert(tag->getTypeForDecl());
    }
  }

  /// Recurse down through an entire translation unit, looking for
  /// "interesting" expressions (e.g. assignments to fields whose types have
  /// been tagged with the __tesla attribute).
  virtual void HandleTranslationUnit(ASTContext &ctx) {
    // Print out the list of types that we will annotate assignments to.
    llvm::errs() << "Tags to instrument with TESLA assertions:\n";
    for (set<const Type*>::const_iterator i = toInstrument.begin();
         i != toInstrument.end(); i++)
      llvm::errs()
        << "  " << (*i)->getCanonicalTypeInternal().getAsString()
        << "\n";
    llvm::errs() << "\n";

    // Find the 'struct tesla_data' type.
    const vector<Type*> types = ctx.getTypes();
    typedef vector<Type*>::const_iterator Iterator;
    for (Iterator i = types.begin(); i != types.end(); i++) {
      const QualType pointee = (*i)->getPointeeType();
      const QualType qualType = (*i)->getCanonicalTypeInternal();

      if (RecordType *rec = dyn_cast<RecordType>(*i)) {
        RecordDecl *decl = rec->getDecl();
        if (decl->getName() == "tesla_data") {
          teslaDataType = rec->getCanonicalTypeInternal();
          break;
        }
      }
    }

    if (teslaDataType.isNull()) {
      Diagnostic &D = ctx.getDiagnostics();
      unsigned DiagID = D.getCustomDiagID(
        Diagnostic::Error, "'struct tesla_data' not defined");
      D.Report(DiagID);
    }

    // Start recursing down!
    TranslationUnitDecl *tu = ctx.getTranslationUnitDecl();
    VisitDecl(tu, tu, ctx);
    }
};


class TeslaAction : public PluginASTAction {
protected:
  ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) {
    return new TeslaInstrumenter();
  }

  bool ParseArgs(const CompilerInstance &CI, const vector<string>& args) {
    for (unsigned i = 0, e = args.size(); i != e; ++i) {
      llvm::errs() << "TeslaInstrumenter arg = " << args[i] << "\n";

      // Example error handling.
      if (args[i] == "-an-error") {
        Diagnostic &D = CI.getDiagnostics();
        unsigned DiagID = D.getCustomDiagID(
          Diagnostic::Error, "invalid argument '" + args[i] + "'");
        D.Report(DiagID);
        return false;
      }
    }
    if (args.size() and args[0] == "help")
      PrintHelp(llvm::errs());

    return true;
  }

  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Help for TeslaInstrumenter plugin goes here\n";
  }
};

static FrontendPluginRegistry::Add<TeslaAction>
X("tesla", "Add TESLA instrumentation");


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
