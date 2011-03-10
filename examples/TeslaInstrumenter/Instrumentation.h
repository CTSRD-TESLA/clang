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

    /// Inserts the instrumentation before a particular Stmt.
    void insert(clang::CompoundStmt *c, const clang::Stmt *before,
        clang::ASTContext &ast);

    /// Inserts the instrumentation at the beginning of a CompoundStmt.
    void insert(clang::CompoundStmt *c, clang::ASTContext &ast) {
      insert(c, *c->children(), ast);
    }

    /// Appends the instrumentation to the end of a CompoundStmt.
    void append(clang::CompoundStmt *c, clang::ASTContext &ast);
};


/// A value is being assigned to a structure of interest.
class FieldAssignment : public Instrumentation {
  private:
    const static std::string PREFIX;

    clang::MemberExpr *lhs;
    clang::Expr *rhs;
    clang::QualType structType;
    clang::FieldDecl *field;

    /// The name of the function which we will call to check this assignment.
    std::string checkerName() const;

  public:
    FieldAssignment(clang::MemberExpr *lhs, clang::Expr *rhs);
    virtual clang::Stmt* create(clang::ASTContext &ast);
};

#endif // TESLA_INSTRUMENTATION_H
