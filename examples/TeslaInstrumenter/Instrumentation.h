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
    virtual clang::Stmt* create(clang::ASTContext &ast) = 0;

    void insertBefore(
        clang::CompoundStmt *c,
        const clang::Stmt *before,
        clang::ASTContext &ast);

    void insert(clang::CompoundStmt *c, clang::ASTContext &ast) {
      insertBefore(c, *c->children(), ast);
    }
};


/// A hook to call out to an external checker.
class AssignHook : public Instrumentation {
  private:
    const static std::string PREFIX;

    clang::MemberExpr *lhs;
    clang::Expr *rhs;
    clang::QualType structType;
    clang::FieldDecl *field;

    /// The name of the function which we will call to check this assignment.
    std::string checkerName() const;

  public:
    AssignHook(clang::MemberExpr *lhs, clang::Expr *rhs);
    virtual clang::Stmt* create(clang::ASTContext &ast);
};

#endif // TESLA_INSTRUMENTATION_H
