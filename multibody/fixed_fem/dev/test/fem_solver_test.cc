#include "drake/multibody/fixed_fem/dev/fem_solver.h"

#include <gtest/gtest.h>

#include "drake/common/test_utilities/eigen_matrix_compare.h"
#include "drake/geometry/proximity/make_box_mesh.h"
#include "drake/math/autodiff_gradient.h"
#include "drake/multibody/fixed_fem/dev/eigen_conjugate_gradient_solver.h"
#include "drake/multibody/fixed_fem/dev/fem_state.h"
#include "drake/multibody/fixed_fem/dev/linear_constitutive_model.h"
#include "drake/multibody/fixed_fem/dev/linear_simplex_element.h"
#include "drake/multibody/fixed_fem/dev/simplex_gaussian_quadrature.h"
#include "drake/multibody/fixed_fem/dev/static_elasticity_element.h"
#include "drake/multibody/fixed_fem/dev/static_elasticity_model.h"
#include "drake/multibody/fixed_fem/dev/zeroth_order_state_updater.h"

namespace drake {
namespace multibody {
namespace fixed_fem {
constexpr int kNaturalDimension = 3;
constexpr int kSpatialDimension = 3;
constexpr int kSolutionDimension = 3;
constexpr int kQuadratureOrder = 1;
/* Number of nodes subject to Dirichlet BC. */
constexpr int kNumDirichlet = 4;
constexpr int kNumNodes = 8;
constexpr int kNumDofs = kNumNodes * kSolutionDimension;
constexpr double kTol = 1e-14;
using T = double;
using QuadratureType =
    SimplexGaussianQuadrature<kNaturalDimension, kQuadratureOrder>;
constexpr int kNumQuads = QuadratureType::num_quadrature_points();
using IsoparametricElementType =
    LinearSimplexElement<T, kNaturalDimension, kSpatialDimension, kNumQuads>;
using ConstitutiveModelType = LinearConstitutiveModel<T, kNumQuads>;
using ElementType =
    StaticElasticityElement<IsoparametricElementType, QuadratureType,
                            ConstitutiveModelType>;
using ModelType = StaticElasticityModel<ElementType>;
using SolverType = FemSolver<ModelType>;
using State = FemState<ElementType>;
const double kYoungsModulus = 1.234;
const double kPoissonRatio = 0.4567;
const double kDensity = 8.90;
/* Set a non-default gravity to check gravity's set correctly. */
const Vector3<T> kGravity{1.23, -4.56, 7.89};

class FemSolverTest : public ::testing::Test {
 protected:
  /* Make a box and subdivide it into 6 tetrahedra. */
  static geometry::VolumeMesh<T> MakeBoxTetMesh() {
    const double kLength = 0.1;
    const geometry::Box box(kLength, kLength, kLength);
    const geometry::VolumeMesh<T> mesh =
        geometry::internal::MakeBoxVolumeMesh<T>(box, kLength);
    EXPECT_EQ(mesh.num_elements(), 6);
    EXPECT_EQ(mesh.num_vertices(), 8);
    return mesh;
  }

  void SetUp() override {
    /* Builds the FemModel. */
    std::unique_ptr<ModelType> model = MakeBoxModel();
    model->SetGravity(kGravity);

    /* Builds the FemSolver. */
    solver_ = std::make_unique<SolverType>(std::move(model));
    solver_->set_linear_solve_tolerance(kTol);
    solver_->set_relative_tolerance(kTol);
    solver_->set_absolute_tolerance(kTol);

    /* Set up the Dirichlet BC. */
    std::unique_ptr<DirichletBoundaryCondition<State>> bc = MakeCeilingBc();
    solver_->SetDirichletBoundaryCondition(std::move(bc));
  }

  static std::unique_ptr<ModelType> MakeBoxModel() {
    const geometry::VolumeMesh<T> mesh = MakeBoxTetMesh();
    const ConstitutiveModelType constitutive_model(kYoungsModulus,
                                                   kPoissonRatio);
    auto model = std::make_unique<ModelType>();
    model->AddStaticElasticityElementsFromTetMesh(mesh, constitutive_model,
                                                  kDensity);
    return model;
  }

  /* Creates the undeformed state of the model under test. */
  static State MakeReferenceState() {
    std::unique_ptr<ModelType> model = MakeBoxModel();
    return model->MakeFemState();
  }

  /* Creates a Dirichlet boundary condition that constrains the first
   `kNumDirichlet` vertices. */
  static std::unique_ptr<DirichletBoundaryCondition<State>> MakeCeilingBc() {
    auto bc = std::make_unique<DirichletBoundaryCondition<State>>();
    const State state = MakeReferenceState();
    const VectorX<T>& q = state.q();
    for (int node = 0; node < kNumDirichlet; ++node) {
      const DofIndex starting_dof_index(kSolutionDimension * node);
      /* Dirichlet BC for all dofs associated with the node. */
      for (int d = 0; d < kSolutionDimension; ++d) {
        bc->AddBoundaryCondition(DofIndex(starting_dof_index + d),
                                 Vector1<T>(q(starting_dof_index + d)));
      }
    }
    return bc;
  }

  /* Creates an arbitrary state that respects the Dirichlet BC created above. */
  static State MakeArbitraryState() {
    State state = MakeReferenceState();
    const VectorX<T> q = MakeArbitraryPositions();
    state.set_q(q);
    std::unique_ptr<DirichletBoundaryCondition<State>> bc = MakeCeilingBc();
    bc->ApplyBoundaryConditions(&state);
    return state;
  }

  /* Creates an arbitrary position vector with dofs compatible with the box
   mesh. The positions for this test are indeed arbitrarily chosen and do not
   necessarily lead to a valid physical configuration. Whether the configuration
   is physical is not relevant because the static linear elasticity test below
   should pass *regardless* of the geometry state. */
  static Vector<double, kNumDofs> MakeArbitraryPositions() {
    Vector<double, kNumDofs> q;
    q << 0.18, 0.63, 0.54, 0.13, 0.92, 0.17, 0.03, 0.86, 0.85, 0.25, 0.53, 0.67,
        0.81, 0.36, 0.45, 0.31, 0.29, 0.71, 0.30, 0.68, 0.58, 0.52, 0.35, 0.76;
    return q;
  }

  /* The solver under test. */
  std::unique_ptr<SolverType> solver_;
};

namespace {
/* We move the vertices of the mesh to arbitrary locations q and record the net
force f exerted on the vertices. Then if we apply f on the vertices in the
reference state, we should recover the positions q for static equilibrium. */
TEST_F(FemSolverTest, StaticForceEquilibrium) {
  /* Create an arbitrary state and find the nodel force exerted on the vertices
   of the mesh (in unit N). */
  const State prescribed_state = MakeArbitraryState();
  const ModelType& model = solver_->model();
  VectorX<T> nodal_force(kNumDofs);
  model.CalcResidual(prescribed_state, &nodal_force);

  /* If we exert the same force on the reference state, we should expect to
   recover the same positions as above. */
  const T initial_error = nodal_force.norm();
  ModelType& mutable_model = solver_->mutable_model();
  mutable_model.SetExplicitExternalForce(nodal_force);
  State state = MakeReferenceState();
  const ZerothOrderStateUpdater<State> state_updater;
  solver_->SolveWithInitialGuess(state_updater, &state);
  EXPECT_TRUE(CompareMatrices(state.q(), prescribed_state.q(),
                              std::max(kTol, kTol * initial_error)));
}
}  // namespace
}  // namespace fixed_fem
}  // namespace multibody
}  // namespace drake
