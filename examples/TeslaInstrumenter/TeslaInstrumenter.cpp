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

/// \file TeslaInstrumenter and TeslaAction

#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/Frontend/CompilerInstance.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

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
  /// Write a header file which can be used to type-check event handlers.
  void writeInstrumentationHeader(ostream& os, const vector<Stmt*>&);

  /// Write a set of variables which can be used to generate event handling
  /// code from a parameterized template.
  void writeTemplateVars(llvm::raw_ostream& os, const vector<TeslaAssertion>&);

  // Write a .spec file based on referenced functions
  void writeSpec(llvm::raw_ostream& os, const vector<TeslaAssertion>&);

  // Write a instrumentation .c file based on referenced functions
  void writeInstrumentation(llvm::raw_ostream& os, const vector<TeslaAssertion>&);

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

  vector<TeslaAssertion> assertions;
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
  teslaDataType = ast.VoidPtrTy;
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
}

void TeslaInstrumenter::HandleTranslationUnit(ASTContext &ast) {
  if (instrumentation.size() > 0) {
    string headerFileName = (filename + "-tesla.h").str();
    ofstream headerFile(headerFileName.c_str());
    writeInstrumentationHeader(headerFile, instrumentation);
  }

  if (assertions.size() > 0) {
    string varFileName = (filename + ".vars").str();
    ofstream varFile(varFileName.c_str());
    llvm::raw_os_ostream raw(varFile);
    writeTemplateVars(raw, assertions);
  }

  if (assertions.size() > 0) {
    string varFileName = "generated.spec";
    ofstream varFile(varFileName.c_str());
    llvm::raw_os_ostream raw(varFile);
    writeSpec(raw, assertions);
  }

  if (assertions.size() > 0) {
    string varFileName = "generated.c";
    ofstream varFile(varFileName.c_str());
    llvm::raw_os_ostream raw(varFile);
    writeInstrumentation(raw, assertions);
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

      // Always instrument the prologue.
      store(FunctionEntry(f, teslaDataType).insert(cs, ast));

      // Only instrument the epilogue of void functions. If return statements
      // keep us from getting here, this will be dead code, but it will be
      // pruned in CG (including IR generation).
      if (f->getResultType() == ast.VoidTy)
        warnAddingInstrumentation(f->getLocEnd()) << f->getSourceRange();
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
    assertions.push_back(tesla);
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
      // To have a non-Expr child of an Expr is... odd.
      int id = diag->getCustomDiagID(
          Diagnostic::Warning, "Expression has Non-Expr child");

      diag->Report(id) << e->getSourceRange();

      // The FreeBSD kernel does this kind of thing, though:
#if 0
#define VFS_LOCK_GIANT(MP) __extension__                                \
({                                                                      \
        int _locked;                                                    \
        struct mount *_mp;                                              \
        _mp = (MP);                                                     \
        if (VFS_NEEDSGIANT_(_mp)) {                                     \
                mtx_lock(&Giant);                                       \
                _locked = 1;                                            \
        } else                                                          \
                _locked = 0;                                            \
        _locked;                                                        \
})
#endif

      // Until we figure out exactly what has been segfaulting, we
      // choose to ignore such expressions. Hopefully we never want to
      // instrument them.
//      Visit(s, f, cs, dc, ast);
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



void TeslaInstrumenter::writeInstrumentationHeader(
    ostream& os, const vector<Stmt*>& instrumentation) {

  // Print out a header file for instrumentation event handlers.
  for (vector<Stmt*>::const_iterator i = instrumentation.begin();
       i != instrumentation.end(); i++)
    if (CallExpr *call = dyn_cast<CallExpr>(*i)) {

      os
        << call->getType().getAsString() << " "
        << call->getDirectCallee()->getName().str() << "(";

      for (size_t i = 0; i < call->getNumArgs();) {
        os << call->getArg(i)->getType().getAsString();
        if (++i != call->getNumArgs()) os << ", ";
      }

      os << ");\n";
    }
}


/// A quick'n'dirty way to refer to a possibly-not-yet-instantiated output.
///
/// Such things make me yearn for Python and its collections.defaultdict().
#define var(name) \
  ((vars.count(name) > 0) ? *vars[name] : *(vars[name] = new ostringstream()))

#define flush() \
  for (map<string, ostringstream*>::iterator i = vars.begin(); \
       i != vars.end(); i++) { \
    out_stream << i->first << ":" << i->second->str() << "\n"; \
    delete i->second; \
  } \
  vars.clear();

void TeslaInstrumenter::writeSpec(
    llvm::raw_ostream& out_stream, const vector<TeslaAssertion>& assertions) {

  for (vector<TeslaAssertion>::const_iterator i = assertions.begin();
       i != assertions.end(); i++) {

    const TeslaAssertion& a = *i;
    const TeslaAssertion::FunctionParamMap& fns = a.getReferencedFunctions();
    typedef TeslaAssertion::FunctionParamMap::const_iterator FPIterator;

    // iterate over each function and its parameters
    for (FPIterator i = fns.begin(); i != fns.end(); i++) {
      FunctionDecl *fn = i->first;

      out_stream << "function," << fn->getName() << "\n";
    }
  }
}

void TeslaInstrumenter::writeInstrumentation(
    llvm::raw_ostream& out_stream, const vector<TeslaAssertion>& assertions) {

  for (vector<TeslaAssertion>::const_iterator i = assertions.begin();
       i != assertions.end(); i++) {

    const TeslaAssertion& a = *i;

    out_stream << "void " << a.getName() << "(){ ; }\n";

    const TeslaAssertion::FunctionParamMap& fns = a.getReferencedFunctions();
    typedef TeslaAssertion::FunctionParamMap::const_iterator FPIterator;

    // iterate over each function and its parameters
    for (FPIterator i = fns.begin(); i != fns.end(); i++) {
      FunctionDecl *fn = i->first;
      const vector<Expr*>& params = i->second;

      // XXX: Use ___tesla_event constant where possible
      out_stream << "void __tesla_event_function_prologue_" << fn->getName() \
	<< "(void **tesla_data";
      for (size_t j = 0; j < params.size(); j++) {
        // XXX: refactor from writeTemplateVars
        DeclRefExpr *dre = dyn_cast<DeclRefExpr>(params[j]->IgnoreParenCasts());
        if (dre == NULL) continue;

        ValueDecl *decl = dyn_cast<ValueDecl>(dre->getDecl());
        if (decl == NULL) continue;
        if (decl->getName() == "__tesla_dont_care") continue;

        string name = decl->getName().str();
        string typeName = decl->getType().getAsString();
        out_stream << ", " << typeName << " " << name;
      }
      out_stream << ") { ; } \n";
      out_stream << "void __tesla_event_function_return_" << fn->getName() \
	<< "(void **tesla_data, int retval) { ; } \n";
    }
  }
}

void TeslaInstrumenter::writeTemplateVars(
    llvm::raw_ostream& out_stream, const vector<TeslaAssertion>& assertions) {

  for (vector<TeslaAssertion>::const_iterator i = assertions.begin();
       i != assertions.end(); i++) {

    map<string, ostringstream*> vars;

    const TeslaAssertion& a = *i;
    out_stream
      << "#\n"
      << "# assertion: " << a.getName() << "\n"
      << "#\n";

    // the name of the function containing the assertion
    var("ASSERT_FN") << a.getDeclaringFunction()->getName().str();

    // the assertion event handler
    var("ASSERTION_EVENT") << a.getName() << "(";
    for (size_t i = 0; i < a.getVariableCount();) {
      const ValueDecl *var = a.getVariable(i);
      var("ASSERTION_EVENT")
        << var->getType().getAsString() << " " << var->getName().str()
        << ((++i == a.getVariableCount()) ? "" : ", ");
    }
    var("ASSERTION_EVENT") << ")";

    // automata storage: global or thread-local
    var("STORAGE")
      << "TESLA_SCOPE_"
      << ((a.getStorageClass() == TeslaAssertion::GLOBAL)
          ? "GLOBAL" : "PERTHREAD");

    var("ASSERT_SCOPE_BEGIN") << a.getScopeBegin()->getNameInfo().getAsString();
    var("ASSERT_SCOPE_END") << a.getScopeEnd()->getNameInfo().getAsString();

    flush();

    // the longest parameter list
    size_t maxPos = 0;

    const TeslaAssertion::FunctionParamMap& fns = a.getReferencedFunctions();
    typedef TeslaAssertion::FunctionParamMap::const_iterator FPIterator;

    // iterate over each function and its parameters
    for (FPIterator i = fns.begin(); i != fns.end(); i++) {
      FunctionDecl *fn = i->first;
      const vector<Expr*>& params = i->second;

      out_stream << "\n# '" << fn->getName() << "()' events:\n";

      var("INSTRUMENTED_FN") << fn->getName().str();

      size_t pos = 1;
      for (size_t j = 0; j < params.size(); j++) {
        DeclRefExpr *dre = dyn_cast<DeclRefExpr>(params[j]->IgnoreParenCasts());
        if (dre == NULL) continue;

        ValueDecl *decl = dyn_cast<ValueDecl>(dre->getDecl());
        if (decl == NULL) continue;
        if (decl->getName() == "__tesla_dont_care") continue;

        string name = decl->getName().str();
        string typeName = decl->getType().getAsString();
        string comma = ((j + 1) == params.size()) ? "" : ", ";

        // Values used to look up automata, provided by either the event
        // itself (e.g. __tesla_event_assertion(struct ucred* u)) or
        // previously-stored state (see {STORE,EXTRACT}_STATE, below).
        var("KEYARGS") << "(register_t) " << name << comma;

        // Declaration of KEYARGS (e.g. in a function which extracts this
        // data from a tesla_instance).
        var("KEYARGS_DECL")
          << "\t" << typeName << " " << name << ";$";

        // Code to store function parameters in a tesla_instance.
        var("STORE_STATE")
          << "\ttip->ti_state[" << pos
          << "] = (register_t) " << name << ";$";

        // Code to extract state (e.g. invocation params) from a tesla_instance.
        var("EXTRACT_STATE")
          << "\t" << name << " = (" << typeName << ") "
          << "tip->ti_state[" << pos << "];$";

        // Parameters to the __tesla_event_function_prologue() event.
        var("FN_ENTER_PARAMS") << typeName << " " << name << comma;

        if (pos > maxPos) maxPos = pos;
        pos++;
      }

      var("NUMARGS") << pos;

      flush();
    }

    maxPos++;     // Convert largest index into array size
    var("COMPILE_TIME_CHECKS")
      << "#if (TESLA_STATE_SIZE <= " << maxPos << ")$"
      << "#error TESLA_STATE_SIZE is too small (need " << maxPos << ")$"
      << "#endif"
      ;

    out_stream << "\n# static typechecking stuff\n";
    flush();
    out_stream
      << "#\n"
      << "# end of assertion " << a.getName() << "\n"
      << "#\n"
      << "\n";
  }
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
