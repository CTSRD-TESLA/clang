/// \file TeslaInstrumenter and TeslaAction

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <map>
#include <set>

#include "Instrumentation.h"

using namespace clang;
using namespace std;

namespace {

typedef map<string, vector<string> > FieldMap;


/// Instruments assignments to tag fields with TESLA assertions.
class TeslaInstrumenter : public ASTConsumer {
private:
  QualType teslaDataType;

  Diagnostic *diag;
  unsigned int teslaWarningId;

  const vector<string> typesToInstrument;
  FieldMap fieldsToInstrument;

  const vector<string> functionsToInstrument;

  template<class T>
  bool contains(const vector<T>& haystack, const T& needle) const {
    return (find(haystack.begin(), haystack.end(), needle) != haystack.end());
  }

  bool needToInstrument(const FunctionDecl *f) const {
    if (f == NULL) return false;
    return contains<string>(functionsToInstrument, f->getName().str());
  }

  bool needToInstrument(const FieldDecl *field) const {
    const RecordDecl *record = field->getParent();
    string base = QualType::getAsString(record->getTypeForDecl(), Qualifiers());

    FieldMap::const_iterator i = fieldsToInstrument.find(base);
    if (i == fieldsToInstrument.end()) return false;

    return contains<string>(i->second, field->getName().str());
  }

  DiagnosticBuilder warnAddingInstrumentation(SourceLocation) const;


public:
  TeslaInstrumenter(
      FieldMap fieldsToInstrument,
      vector<string> functionsToInstrument);

  void Visit(DeclContext *dc, ASTContext &ast);
  void Visit(Decl *d, DeclContext *context, ASTContext &ast);
  void Visit(CompoundStmt *cs, FunctionDecl *f, DeclContext* context,
      ASTContext &ast);
  void Visit(Stmt *s, FunctionDecl *f, CompoundStmt *cs, DeclContext* context,
      ASTContext &ast);
  void Visit(ReturnStmt *r, CompoundStmt *cs, FunctionDecl *f, DeclContext *dc,
      ASTContext& ast);
  void Visit(Expr *e, FunctionDecl *f, Stmt *s, CompoundStmt *c,
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
      return new TeslaInstrumenter(fields, functions);
    }

    bool ParseArgs(const CompilerInstance &CI, const vector<string>& args);
    void PrintHelp(llvm::raw_ostream& ros);

  private:
    map<string, vector<string> > fields;
    vector<string> functions;
};

static FrontendPluginRegistry::Add<TeslaAction>
X("tesla", "Add TESLA instrumentation");



// ********* TeslaInstrumenter (still in the anonymous namespace). ********

TeslaInstrumenter::TeslaInstrumenter(
      map<string, vector<string> > fieldsToInstrument,
      vector<string> functionsToInstrument)
  : fieldsToInstrument(fieldsToInstrument),
    functionsToInstrument(functionsToInstrument) {
}

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
    assert(isa<RecordDecl>(tag) && "Can't instrument funny tags like enums");
    RecordDecl *r = dyn_cast<RecordDecl>(tag);
    string typeName = QualType::getAsString(r->getTypeForDecl(), Qualifiers());

    typedef RecordDecl::field_iterator FieldIterator;
    for (FieldIterator i = r->field_begin(); i != r->field_end(); i++) {
      fieldsToInstrument[typeName].push_back((*i)->getName());
    }
  }

  if (tag->getName() == "__tesla_data")
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
  FunctionDecl *f = dyn_cast<FunctionDecl>(d);
  if ((f != NULL) and !f->isThisDeclarationADefinition()) return;

  if (DeclContext *dc = dyn_cast<DeclContext>(d)) {
    Visit(dc, ast);
    context = dc;
  }

  if (d->hasBody()) {
    assert(isa<CompoundStmt>(d->getBody()));
    CompoundStmt *cs = dyn_cast<CompoundStmt>(d->getBody());

    if (needToInstrument(f)) {
      SourceRange fRange = f->getSourceRange();
      warnAddingInstrumentation(fRange.getBegin()) << fRange;

      // Always instrument the prologue.
      FunctionEntry(f, teslaDataType).insert(cs, ast);

      // Only instrument the epilogue of void functions. If return statements
      // keep us from getting here, this will be dead code, but it will be
      // pruned in CG (including IR generation).
      if (f->getResultType() == ast.VoidTy)
        FunctionReturn(f, NULL).append(cs, ast);
    }

    Visit(cs, f, context, ast);
  }
}

void TeslaInstrumenter::Visit(
    CompoundStmt *cs, FunctionDecl *f, DeclContext* dc, ASTContext &ast) {

  assert(cs != NULL);

  for (StmtRange child = cs->children(); child; child++) {
    // It's perfectly legitimate to have a null child (think an IfStmt with
    // no 'else' clause), but dyn_cast() will choke on it.
    if (*child == NULL) continue;

    if (Expr *e = dyn_cast<Expr>(*child)) Visit(e, f, e, cs, dc, ast);
    else Visit(*child, f, cs, dc, ast);
  }
}

void TeslaInstrumenter::Visit(
    Stmt *s, FunctionDecl *f, CompoundStmt *cs, DeclContext* dc,
    ASTContext &ast) {

  assert(s != NULL);

  if (CompoundStmt *c = dyn_cast<CompoundStmt>(s)) Visit(c, f, dc, ast);
  else if (ReturnStmt *r = dyn_cast<ReturnStmt>(s)) Visit(r, cs, f, dc, ast);
  else
    for (StmtRange child = s->children(); child; child++) {
      // It's perfectly legitimate to have a null child (think an IfStmt with
      // no 'else' clause), but dyn_cast() will choke on it.
      if (*child == NULL) continue;

      if (Expr *e = dyn_cast<Expr>(*child)) Visit(e, f, s, cs, dc, ast);
      else Visit(*child, f, cs, dc, ast);
    }
}

void TeslaInstrumenter::Visit(ReturnStmt *r, CompoundStmt *cs, FunctionDecl *f,
    DeclContext *dc, ASTContext& ast) {

  if (needToInstrument(f)) {
    warnAddingInstrumentation(r->getLocStart()) << r->getSourceRange();
    FunctionReturn(f, r->getRetValue()).insert(cs, r, ast);
  }
}

void TeslaInstrumenter::Visit(Expr *e, FunctionDecl *f, Stmt *s,
    CompoundStmt *cs, DeclContext* dc, ASTContext &ast) {

  assert(e != NULL);

  if (BinaryOperator *o = dyn_cast<BinaryOperator>(e))
    Visit(o, s, cs, ast);

  for (StmtRange child = e->children(); child; child++) {
    assert(isa<Expr>(*child) && "Non-Expr child of Expr");
    Visit(dyn_cast<Expr>(*child), f, s, cs, dc, ast);
  }
}

void TeslaInstrumenter::Visit(
    BinaryOperator *o, Stmt *s, CompoundStmt *cs, ASTContext &ast) {

  if (!o->isAssignmentOp()) return;

  // We only care about assignments to structure fields.
  MemberExpr *lhs = dyn_cast<MemberExpr>(o->getLHS());
  if (lhs == NULL) return;

  // Do we want to instrument this type?
  assert(isa<FieldDecl>(lhs->getMemberDecl()));
  if (!needToInstrument(dyn_cast<FieldDecl>(lhs->getMemberDecl()))) return;

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

  FieldAssignment hook(lhs, rhs);
  warnAddingInstrumentation(o->getLocStart()) << o->getSourceRange();
  hook.insert(cs, s, ast);
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


// ********* TeslaInstrumenter (still in the anonymous namespace). ********
bool
TeslaAction::ParseArgs(const CompilerInstance &CI, const vector<string>& args) {
  if (args.size() != 1) {
    PrintHelp(llvm::errs());
    return false;
  }

  ifstream specFile(args[0].c_str());
  if (!specFile.is_open()) {
    llvm::errs() << "Failed to open spec file '" << args[0] << "'";
    return false;
  }

  while (specFile.good()) {
    string line;
    getline(specFile, line);

    vector<string> args;
    for (size_t i = 0; i != string::npos;) {
      size_t j = line.find(",", i);
      args.push_back(line.substr(i, j - i));

      if (j == string::npos) break;
      i = j + 1;
    }

    if (args.size() == 0) continue;

    if (args[0] == "field_assign") {
      if (args.size() != 3) {
        Diagnostic& diag = CI.getDiagnostics();
        int id = diag.getCustomDiagID(
          Diagnostic::Error,
          "'field_assign' line in spec file should have 2 argument");

        diag.Report(id);
        return false;
      }

      fields[args[1]].push_back(args[2]);
    } else if (args[0] == "function") {
      if (args.size() != 2) {
        Diagnostic& diag = CI.getDiagnostics();
        int id = diag.getCustomDiagID(
          Diagnostic::Error,
          "'function' line in spec file should have 1 argument");

        diag.Report(id);
        return false;
      }

      functions.push_back(args[1]);
    }
  }

  return true;
}

void TeslaAction::PrintHelp(llvm::raw_ostream& ros) {
  ros << "tesla usage: -plugin tesla -plugin-arg-tesla <spec file>\n";
}

}
