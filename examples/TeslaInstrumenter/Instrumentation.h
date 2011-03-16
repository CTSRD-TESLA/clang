/// \file Instrumentation and some subclasses.

#ifndef TESLA_INSTRUMENTATION_H
#define TESLA_INSTRUMENTATION_H

#include "clang/AST/AST.h"


/// Some instrumentation code.
///
/// For instance, before assigning to a variable, an Instrumentation instance
/// might call out to a function which says "yes, this is ok."
class Instrumentation {
  public:
    /// Creates the actual instrumentation code.
    virtual std::vector<clang::Stmt*> create(clang::ASTContext &ast) = 0;

    /// Inserts the instrumentation before a particular Stmt.
    void insert(clang::CompoundStmt *c, const clang::Stmt *before,
        clang::ASTContext &ast);

    /// Inserts the instrumentation at the beginning of a CompoundStmt.
    void insert(clang::CompoundStmt *c, clang::ASTContext &ast) {
      if (c->children()) insert(c, *c->children(), ast);
      else append(c, ast);
    }

    /// Replaces a number of statements with instrumentation.
    void replace(clang::CompoundStmt *c, clang::Stmt *s,
        clang::ASTContext &ast, size_t len = 1);

    /// Appends the instrumentation to the end of a CompoundStmt.
    void append(clang::CompoundStmt *c, clang::ASTContext &ast);

    /// The name of the event handler function.
    std::string eventHandlerName(const std::string &suffix) const;

  protected:
  /// Turn an expression into an L-Value.
  ///
  /// When we call instrumentation, we don't want to evaluate expressions
  /// twice (e.g. 'return foo();' -> '__instrument(foo()); return foo();').
  /// Instead, we should create a temporary variable and assign it just before
  /// we would normally take the instrumentated action:
  ///
  /// T *tmp;
  /// ...
  /// tmp = foo();
  /// __instrument(tmp);
  /// return tmp;
  ///
  /// @returns a pair containing 1) an expression which references the temporary
  ///          variable, and 2) the statement which initializes it
  std::pair<clang::Expr*, std::vector<clang::Stmt*> > makeLValue(
      clang::Expr *e, const std::string& name,
      clang::DeclContext *dc, clang::ASTContext &ast,
      clang::SourceLocation location = clang::SourceLocation());

  std::string typeIdentifier(const clang::QualType t) const;
  std::string typeIdentifier(const clang::Type *t) const;

  private:
    const static std::string PREFIX;
};


/// Instruments an assertion's point of declaration.
class TeslaAssertion : public Instrumentation {
  public:
    /// Constructor.
    ///
    /// @param  e           the expression which might be the 'TESLA' marker
    /// @param  cs          the CompoundStmt that e is found in
    /// @param  f           the function the alleged assertion is made in
    /// @param  assertCount how many assertions have already been made in f
    /// @param  d           where to output errors and warnings
    TeslaAssertion(clang::Expr *e, clang::CompoundStmt *cs,
        clang::FunctionDecl *f, int assertCount, clang::Diagnostic& d);

    bool isValid() const {
      return ((parent != NULL) and (marker != NULL) and (assertion != NULL));
    }

    virtual std::vector<clang::Stmt*> create(clang::ASTContext &ast);

  private:
    /// Recursively searches for variable references.
    void searchForVariables(clang::Stmt* s);

    std::string fnName;               ///< function containing declaration
    int assertCount;                  ///< existing assertions in function
    clang::CompoundStmt *parent;      ///< where the assertion lives

    clang::CallExpr *marker;          ///< marks the beginning of an assertion
    clang::CompoundStmt *assertion;   ///< block of assertion "expressions"

    /// varables referenced in the assertion
    std::vector<clang::Expr*> references;
};


/// Instruments entry into a function.
class FunctionEntry : public Instrumentation {
  public:
    /// Constructor.
    ///
    /// @param  function      the function whose scope we are instrumenting
    /// @param  teslaDataType the 'struct __tesla_data' type
    FunctionEntry(clang::FunctionDecl *function, clang::QualType teslaDataType);

    virtual std::vector<clang::Stmt*> create(clang::ASTContext &ast);

  private:
    std::string name;

    clang::FunctionDecl *f;               ///< where we can declare things
    clang::QualType teslaDataType;        ///< the type we store scoped data in
    clang::SourceLocation location;       ///< where we pretend to exist
};


/// Instruments a return from a function.
class FunctionReturn : public Instrumentation {
  public:
    /// Constructor.
    ///
    /// @param  function      the function whose scope we are instrumenting
    /// @param  r             the return statement
    FunctionReturn(clang::FunctionDecl *function, clang::ReturnStmt *r);

    virtual std::vector<clang::Stmt*> create(clang::ASTContext &ast);

  private:
    std::string name;

    clang::FunctionDecl *f;               ///< where we can declare things
    clang::ReturnStmt *r;                 ///< to instrument (NULL if void)
    clang::SourceLocation location;       ///< where we pretend to exist
};

/// A value is being assigned to a structure of interest.
class FieldAssignment : public Instrumentation {
  private:
    clang::MemberExpr *lhs;
    clang::Expr *rhs;
    clang::QualType structType;
    clang::FieldDecl *field;

  public:
    FieldAssignment(clang::MemberExpr *lhs, clang::Expr *rhs);
    virtual std::vector<clang::Stmt*> create(clang::ASTContext &ast);
};

#endif // TESLA_INSTRUMENTATION_H
