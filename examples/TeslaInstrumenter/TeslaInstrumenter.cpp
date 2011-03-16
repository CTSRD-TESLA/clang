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

using llvm::StringRef;

namespace {

typedef map<string, vector<string> > FieldMap;


/// Instruments assignments to tag fields with TESLA assertions.
class TeslaInstrumenter : public ASTConsumer {
public:
  TeslaInstrumenter(StringRef filename,
      FieldMap fieldsToInstrument,
      vector<string> functionsToInstrument);

  // Visitors
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
      BinaryOperator *o, Stmt *s, CompoundStmt *cs, DeclContext *dc,
      ASTContext &ast);

  // Make 'special' statements more amenable to instrumentation.
  void Prepare(IfStmt *s, ASTContext &ast);
  void Prepare(SwitchCase *s, ASTContext &ast);

  /// Adds a 'struct tesla_data' declaration to a CompoundStmt.
  void addTeslaDeclaration(
      CompoundStmt *c, DeclContext *dc, ASTContext &ast);


  // ASTConsumer implementation.

  virtual void Initialize(ASTContext& ast);

  /// Make note if a tag has been tagged with __tesla or the like.
  virtual void HandleTagDeclDefinition(TagDecl *tag);

  /// Recurse down through a declaration of a variable, function, etc.
  virtual void HandleTopLevelDecl(DeclGroupRef d);

  /// We've finished processing an entire translation unit.
  virtual void HandleTranslationUnit(ASTContext &ast);

private:
  /// The file that we're instrumenting.
  StringRef filename;

  /// How many assertions we have already seen in a function.
  map<FunctionDecl*, int> assertionCount;

  template<class T>
  bool contains(const vector<T>& haystack, const T& needle) const {
    return (find(haystack.begin(), haystack.end(), needle) != haystack.end());
  }

  /// Remember that we've added some instrumentation.
  void store(vector<Stmt*> instrumentation);

  /// Do we need to instrument this function?
  bool needToInstrument(const FunctionDecl *f) const {
    if (f == NULL) return false;
    return contains<string>(functionsToInstrument, f->getName().str());
  }

  /// Do we need to instrument this field?
  bool needToInstrument(const FieldDecl *field) const {
    const RecordDecl *record = field->getParent();
    string base = QualType::getAsString(record->getTypeForDecl(), Qualifiers());

    FieldMap::const_iterator i = fieldsToInstrument.find(base);
    if (i == fieldsToInstrument.end()) return false;

    return contains<string>(i->second, field->getName().str());
  }

  /// Emit a warning about inserted instrumentation.
  DiagnosticBuilder warnAddingInstrumentation(SourceLocation) const;

  /// Wrap a non-compound statement in a compound statement
  /// (if the Stmt is already a CompoundStmt, just pass through).
  CompoundStmt* makeCompound(Stmt *s, ASTContext &ast);


  QualType teslaDataType;

  Diagnostic *diag;
  unsigned int teslaWarningId;

  const vector<string> typesToInstrument;
  FieldMap fieldsToInstrument;

  const vector<string> functionsToInstrument;

  vector<Stmt*> instrumentation;
};


class TeslaAction : public PluginASTAction {
  protected:
    ASTConsumer *CreateASTConsumer(CompilerInstance &CI, StringRef filename);

    bool ParseArgs(const CompilerInstance &CI, const vector<string>& args);
    void PrintHelp(llvm::raw_ostream& ros);

  private:
    map<string, vector<string> > fields;
    vector<string> functions;
};

static FrontendPluginRegistry::Add<TeslaAction>
X("tesla", "Add TESLA instrumentation");



// ********* TeslaInstrumenter (still in the anonymous namespace). ********

TeslaInstrumenter::TeslaInstrumenter(StringRef filename,
      FieldMap fieldsToInstrument,
      vector<string> functionsToInstrument)
  : filename(filename), fieldsToInstrument(fieldsToInstrument),
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

void TeslaInstrumenter::HandleTranslationUnit(ASTContext &ast) {
  string headerFileName = ("__tesla_instrumentation_" + filename + ".h").str();
  ofstream output(headerFileName.c_str());

  // Print out a header file for instrumentation event handlers.
  for (vector<Stmt*>::iterator i = instrumentation.begin();
       i != instrumentation.end(); i++)
    if (CallExpr *call = dyn_cast<CallExpr>(*i)) {

      output
        << call->getType().getAsString() << " "
        << call->getDirectCallee()->getName().str() << "(";

      for (size_t i = 0; i < call->getNumArgs();) {
        output << call->getArg(i)->getType().getAsString();
        if (++i != call->getNumArgs()) output << ", ";
      }

      output << ");\n";
    }
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

      if (teslaDataType.isNull()) {
        int err = diag->getCustomDiagID(
          Diagnostic::Error,
          "Instrumenting function entry before struct __tesla_data defined");

        diag->Report(err) << fRange;
        return;
      }

      // Always instrument the prologue.
      store(FunctionEntry(f, teslaDataType).insert(cs, ast));

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

  // Special cases that we need to munge a little bit.
  if (IfStmt *i = dyn_cast<IfStmt>(s)) Prepare(i, ast);
  else if (SwitchCase *c = dyn_cast<SwitchCase>(s)) Prepare(c, ast);

  // Now visit the node or its children, as appropriate.
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

void TeslaInstrumenter::Prepare(IfStmt *s, ASTContext &ast) {
  // For now, simply replace any non-compound then and else clauses with
  // compound versions. We can improve performance through filtering later;
  // right now, we just want to be able to compile more code.
  if (Stmt *t = s->getThen()) s->setThen(makeCompound(t, ast));
  if (Stmt *e = s->getElse()) s->setElse(makeCompound(e, ast));
}

void TeslaInstrumenter::Prepare(SwitchCase *c, ASTContext &ast) {
  Stmt *sub = c->getSubStmt();
  if (sub == NULL) return;

  // Don't wrap an existing compound statement.
  if (isa<CompoundStmt>(sub)) return;

  // Do wrap a non-compound child.
  CompoundStmt *compound = makeCompound(sub, ast);
  if (CaseStmt *cs = dyn_cast<CaseStmt>(c)) cs->setSubStmt(compound);
  else if (DefaultStmt *d = dyn_cast<DefaultStmt>(c)) d->setSubStmt(compound);
  else
    assert(false && "SwitchCase is neither CaseStmt nor DefaultStmt");
}

void TeslaInstrumenter::Visit(ReturnStmt *r, CompoundStmt *cs, FunctionDecl *f,
    DeclContext *dc, ASTContext& ast) {

  if (needToInstrument(f)) {
    warnAddingInstrumentation(r->getLocStart()) << r->getSourceRange();
    store(FunctionReturn(f, r).insert(cs, r, ast));
  }
}

void TeslaInstrumenter::Visit(Expr *e, FunctionDecl *f, Stmt *s,
    CompoundStmt *cs, DeclContext* dc, ASTContext &ast) {

  assert(e != NULL);

  // See if we can identify the start of a Tesla assertion block.
  TeslaAssertion tesla(e, cs, f, assertionCount[f], *diag);
  if (tesla.isValid()) {
    store(tesla.replace(cs, s, ast, 2));
    assertionCount[f]++;
    return;
  }

  // Otherwise, proceed like normal.
  if (BinaryOperator *o = dyn_cast<BinaryOperator>(e))
    Visit(o, s, cs, f, ast);

  for (StmtRange child = e->children(); child; child++) {
    if (*child == NULL) continue;

    if (Expr *expr = dyn_cast<Expr>(*child)) Visit(expr, f, s, cs, dc, ast);
    else {
      int id = diag->getCustomDiagID(
          Diagnostic::Warning, "Non-Expr child of Expr");

      diag->Report(id) << expr->getSourceRange();
      Visit(s, f, cs, dc, ast);
    }
  }
}

void TeslaInstrumenter::Visit(
    BinaryOperator *o, Stmt *s, CompoundStmt *cs, DeclContext *dc,
    ASTContext &ast) {

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

  FieldAssignment hook(lhs, rhs, dc);
  warnAddingInstrumentation(o->getLocStart()) << o->getSourceRange();
  store(hook.insert(cs, s, ast));
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


void TeslaInstrumenter::store(vector<Stmt*> inst) {
  for (vector<Stmt*>::iterator i = inst.begin(); i != inst.end(); i++)
    instrumentation.push_back(*i);
}


CompoundStmt* TeslaInstrumenter::makeCompound(Stmt *s, ASTContext &ast) {
  // Don't need to nest existing compounds.
  if (CompoundStmt *cs = dyn_cast<CompoundStmt>(s)) return cs;

  SourceLocation loc = s->getLocStart();
  return new (ast) CompoundStmt(ast, &s, 1, loc, loc);
}



DiagnosticBuilder
TeslaInstrumenter::warnAddingInstrumentation(SourceLocation loc) const {
  return diag->Report(loc, teslaWarningId);
}


// ********* TeslaAction (still in the anonymous namespace). ********

ASTConsumer* TeslaAction::CreateASTConsumer(CompilerInstance &CI,
    StringRef filename) {
  return new TeslaInstrumenter(filename, fields, functions);
}

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
