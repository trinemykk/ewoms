// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.

  Consult the COPYING file in the top-level source directory of this
  module for the precise wording of the license and the list of
  copyright holders.
*/
/*!
 * \file
 *
 * \copydoc Ewoms::FvBaseLinearizer
 */
#ifndef EWOMS_FV_BASE_LINEARIZER_HH
#define EWOMS_FV_BASE_LINEARIZER_HH

#include "fvbaseproperties.hh"

#include <ewoms/parallel/gridcommhandles.hh>
#include <ewoms/parallel/threadmanager.hh>
#include <ewoms/parallel/threadedentityiterator.hh>
#include <ewoms/disc/common/baseauxiliarymodule.hh>

#include <opm/material/common/Exceptions.hpp>

#include <dune/common/version.hh>
#include <dune/common/fvector.hh>
#include <dune/common/fmatrix.hh>

#include <type_traits>
#include <iostream>
#include <vector>
#include <thread>
#include <set>
#include <exception>   // current_exception, rethrow_exception
#include <mutex>

namespace Ewoms {
// forward declarations
template<class TypeTag>
class EcfvDiscretization;

/*!
 * \ingroup FiniteVolumeDiscretizations
 *
 * \brief The common code for the linearizers of non-linear systems of equations
 *
 * This class assumes that these system of equations to be linearized are stemming from
 * models that use an finite volume scheme for spatial discretization and an Euler
 * scheme for time discretization.
 */
template<class TypeTag>
class FvBaseLinearizer
{
//! \cond SKIP_THIS
    typedef typename GET_PROP_TYPE(TypeTag, Model) Model;
    typedef typename GET_PROP_TYPE(TypeTag, Discretization) Discretization;
    typedef typename GET_PROP_TYPE(TypeTag, Problem) Problem;
    typedef typename GET_PROP_TYPE(TypeTag, Simulator) Simulator;
    typedef typename GET_PROP_TYPE(TypeTag, GridView) GridView;
    typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
    typedef typename GET_PROP_TYPE(TypeTag, Evaluation) Evaluation;
    typedef typename GET_PROP_TYPE(TypeTag, DofMapper) DofMapper;
    typedef typename GET_PROP_TYPE(TypeTag, ElementMapper) ElementMapper;
    typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;

    typedef typename GET_PROP_TYPE(TypeTag, SolutionVector) SolutionVector;
    typedef typename GET_PROP_TYPE(TypeTag, GlobalEqVector) GlobalEqVector;
    typedef typename GET_PROP_TYPE(TypeTag, JacobianMatrix) JacobianMatrix;
    typedef typename GET_PROP_TYPE(TypeTag, EqVector) EqVector;
    typedef typename GET_PROP_TYPE(TypeTag, Constraints) Constraints;
    typedef typename GET_PROP_TYPE(TypeTag, Stencil) Stencil;
    typedef typename GET_PROP_TYPE(TypeTag, ThreadManager) ThreadManager;

    typedef typename GET_PROP_TYPE(TypeTag, GridCommHandleFactory) GridCommHandleFactory;

    typedef Opm::MathToolbox<Evaluation> Toolbox;

    typedef typename GridView::template Codim<0>::Entity Element;
    typedef typename GridView::template Codim<0>::Iterator ElementIterator;

    typedef GlobalEqVector Vector;
    typedef JacobianMatrix Matrix;

    enum { numEq = GET_PROP_VALUE(TypeTag, NumEq) };
    enum { historySize = GET_PROP_VALUE(TypeTag, TimeDiscHistorySize) };

    typedef Dune::FieldMatrix<Scalar, numEq, numEq> MatrixBlock;
    typedef Dune::FieldVector<Scalar, numEq> VectorBlock;

    static const bool linearizeNonLocalElements = GET_PROP_VALUE(TypeTag, LinearizeNonLocalElements);

    // copying the linearizer is not a good idea
    FvBaseLinearizer(const FvBaseLinearizer&);
//! \endcond

#ifdef _OPENMP
	// to avoid a race condition if two threads handles an exception at
	// the same time, we use an explicit lock to control access to the
	// exception storage amongst thread-local handlers
	std::mutex exceptionLock;
#endif

public:
    FvBaseLinearizer()
    {
        simulatorPtr_ = 0;

        matrix_ = 0;
    }

    ~FvBaseLinearizer()
    {
        delete matrix_;
        auto it = elementCtx_.begin();
        const auto& endIt = elementCtx_.end();
        for (; it != endIt; ++it)
            delete *it;
    }

    /*!
     * \brief Register all run-time parameters for the Jacobian linearizer.
     */
    static void registerParameters()
    { }

    /*!
     * \brief Initialize the linearizer.
     *
     * At this point we can assume that all objects in the simulator
     * have been allocated. We cannot assume that they are fully
     * initialized, though.
     *
     * \copydetails Doxygen::simulatorParam
     */
    void init(Simulator& simulator)
    {
        simulatorPtr_ = &simulator;
        delete matrix_; // <- note that this even works for nullpointers!
        matrix_ = 0;
    }

    /*!
     * \brief Causes the Jacobian matrix to be recreated from scratch before the next
     *        iteration.
     *
     * This method is usally called if the sparsity pattern has changed for some
     * reason. (e.g. by modifications of the grid or changes of the auxiliary equations.)
     */
    void eraseMatrix()
    {
        delete matrix_; // <- note that this even works for nullpointers!
        matrix_ = 0;
    }

    /*!
     * \brief Linearize the full system of non-linear equations.
     *
     * This means the spatial domain plus all auxiliary equations.
     */
    void linearize()
    {
        linearizeDomain();
        linearizeAuxiliaryEquations();
    }

    /*!
     * \brief Linearize the part of the non-linear system of equations that is associated
     *        with the spatial domain.
     *
     * That means that the global Jacobian of the residual is assembled and the residual
     * is evaluated for the current solution.
     *
     * The current state of affairs (esp. the previous and the current solutions) is
     * represented by the model object.
     */
    void linearizeDomain()
    {
        // we defer the initialization of the Jacobian matrix until here because the
        // auxiliary modules usually assume the problem, model and grid to be fully
        // initialized...
        if (!matrix_)
            initFirstIteration_();

        int succeeded;
        try {
            linearize_();
            succeeded = 1;
        }
#if ! DUNE_VERSION_NEWER(DUNE_COMMON, 2,5)
        catch (const Dune::Exception& e)
        {
            std::cout << "rank " << simulator_().gridView().comm().rank()
                      << " caught an exception while linearizing:" << e.what()
                      << "\n"  << std::flush;
            succeeded = 0;
        }
#endif
        catch (const std::exception& e)
        {
            std::cout << "rank " << simulator_().gridView().comm().rank()
                      << " caught an exception while linearizing:" << e.what()
                      << "\n"  << std::flush;
            succeeded = 0;
        }
        catch (...)
        {
            std::cout << "rank " << simulator_().gridView().comm().rank()
                      << " caught an exception while linearizing"
                      << "\n"  << std::flush;
            succeeded = 0;
        }
        succeeded = gridView_().comm().min(succeeded);

        if (!succeeded)
            throw Opm::NumericalIssue("A process did not succeed in linearizing the system");
    }

    /*!
     * \brief Linearize the part of the non-linear system of equations that is associated
     *        with the spatial domain.
     */
    void linearizeAuxiliaryEquations()
    {
        auto& model = model_();
        const auto& comm = simulator_().gridView().comm();
        for (unsigned auxModIdx = 0; auxModIdx < model.numAuxiliaryModules(); ++auxModIdx) {
            bool succeeded = true;
            try {
                model.auxiliaryModule(auxModIdx)->linearize(*matrix_, residual_);
            }
            catch (const std::exception& e) {
                succeeded = false;

                std::cout << "rank " << simulator_().gridView().comm().rank()
                          << " caught an exception while linearizing:" << e.what()
                          << "\n"  << std::flush;
            }
#if ! DUNE_VERSION_NEWER(DUNE_COMMON, 2,5)
            catch (const Dune::Exception& e)
            {
                succeeded = false;

                std::cout << "rank " << simulator_().gridView().comm().rank()
                          << " caught an exception while linearizing:" << e.what()
                          << "\n"  << std::flush;
            }
#endif

            succeeded = comm.min(succeeded);

            if (!succeeded)
                throw Opm::NumericalIssue("linearization of an auxilary equation failed");
        }
    }

    /*!
     * \brief Return constant reference to global Jacobian matrix.
     */
    const Matrix& matrix() const
    { return *matrix_; }

    Matrix& matrix()
    { return *matrix_; }

    /*!
     * \brief Return constant reference to global residual vector.
     */
    const GlobalEqVector& residual() const
    { return residual_; }

    GlobalEqVector& residual()
    { return residual_; }

    /*!
     * \brief Returns the map of constraint degrees of freedom.
     *
     * (This object is only non-empty if the EnableConstraints property is true.)
     */
    const std::map<unsigned, Constraints>& constraintsMap() const
    { return constraintsMap_; }

private:
    Simulator& simulator_()
    { return *simulatorPtr_; }
    const Simulator& simulator_() const
    { return *simulatorPtr_; }

    Problem& problem_()
    { return simulator_().problem(); }
    const Problem& problem_() const
    { return simulator_().problem(); }

    Model& model_()
    { return simulator_().model(); }
    const Model& model_() const
    { return simulator_().model(); }

    const GridView& gridView_() const
    { return problem_().gridView(); }

    const ElementMapper& elementMapper_() const
    { return model_().elementMapper(); }

    const DofMapper& dofMapper_() const
    { return model_().dofMapper(); }

    void initFirstIteration_()
    {
        // initialize the BCRS matrix for the Jacobian of the residual function
        createMatrix_();

        // initialize the Jacobian matrix and the vector for the residual function
        (*matrix_) = 0.0;
        residual_.resize(model_().numTotalDof());
        residual_ = 0;

        // create the per-thread context objects
        elementCtx_.resize(ThreadManager::maxThreads());
        for (unsigned threadId = 0; threadId != ThreadManager::maxThreads(); ++ threadId)
            elementCtx_[threadId] = new ElementContext(simulator_());
    }

    // Construct the BCRS matrix for the Jacobian of the residual function
    void createMatrix_()
    {
        size_t numAllDof =  model_().numTotalDof();

        // allocate raw matrix
        matrix_ = new Matrix(numAllDof, numAllDof, Matrix::random);

        Stencil stencil(gridView_(), model_().dofMapper() );

        // for the main model, find out the global indices of the neighboring degrees of
        // freedom of each primary degree of freedom
        typedef std::set<unsigned> NeighborSet;
        std::vector<NeighborSet> neighbors(numAllDof);
        ElementIterator elemIt = gridView_().template begin<0>();
        const ElementIterator elemEndIt = gridView_().template end<0>();
        for (; elemIt != elemEndIt; ++elemIt) {
            const Element& elem = *elemIt;
            stencil.update(elem);

            for (unsigned primaryDofIdx = 0; primaryDofIdx < stencil.numPrimaryDof(); ++primaryDofIdx) {
                unsigned myIdx = stencil.globalSpaceIndex(primaryDofIdx);

                for (unsigned dofIdx = 0; dofIdx < stencil.numDof(); ++dofIdx) {
                    unsigned neighborIdx = stencil.globalSpaceIndex(dofIdx);
                    neighbors[myIdx].insert(neighborIdx);
                }
            }
        }

        // add the additional neighbors and degrees of freedom caused by the auxiliary
        // equations
        const auto& model = model_();
        size_t numAuxMod = model.numAuxiliaryModules();
        for (unsigned auxModIdx = 0; auxModIdx < numAuxMod; ++auxModIdx)
            model.auxiliaryModule(auxModIdx)->addNeighbors(neighbors);

        // allocate space for the rows of the matrix
        for (unsigned dofIdx = 0; dofIdx < numAllDof; ++ dofIdx)
            matrix_->setrowsize(dofIdx, neighbors[dofIdx].size());
        matrix_->endrowsizes();

        // fill the rows with indices. each degree of freedom talks to
        // all of its neighbors. (it also talks to itself since
        // degrees of freedom are sometimes quite egocentric.)
        for (unsigned dofIdx = 0; dofIdx < numAllDof; ++ dofIdx) {
            typename NeighborSet::iterator nIt = neighbors[dofIdx].begin();
            typename NeighborSet::iterator nEndIt = neighbors[dofIdx].end();
            for (; nIt != nEndIt; ++nIt)
                matrix_->addindex(dofIdx, *nIt);
        }
        matrix_->endindices();
    }

    // reset the global linear system of equations.
    void resetSystem_()
    {
        residual_ = 0.0;
        (*matrix_) = 0.0;
    }

    // query the problem for all constraint degrees of freedom. note that this method is
    // quite involved and is thus relatively slow.
    void updateConstraintsMap_()
    {
        if (!enableConstraints_())
            // constraints are not explictly enabled, so we don't need to consider them!
            return;

        constraintsMap_.clear();

        // loop over all elements...
        ThreadedEntityIterator<GridView, /*codim=*/0> threadedElemIt(gridView_());
#ifdef _OPENMP
#pragma omp parallel
#endif
        {
            unsigned threadId = ThreadManager::threadId();
            ElementIterator elemIt = threadedElemIt.beginParallel();
            for (; !threadedElemIt.isFinished(elemIt); elemIt = threadedElemIt.increment()) {
                // create an element context (the solution-based quantities are not
                // available here!)
                const Element& elem = *elemIt;
                ElementContext& elemCtx = *elementCtx_[threadId];
                elemCtx.updateStencil(elem);

                // check if the problem wants to constrain any degree of the current
                // element's freedom. if yes, add the constraint to the map.
                for (unsigned primaryDofIdx = 0;
                     primaryDofIdx < elemCtx.numPrimaryDof(/*timeIdx=*/0);
                     ++ primaryDofIdx)
                {
                    Constraints constraints;
                    elemCtx.problem().constraints(constraints,
                                                  elemCtx,
                                                  primaryDofIdx,
                                                  /*timeIdx=*/0);
                    if (constraints.isActive()) {
                        unsigned globI = elemCtx.globalSpaceIndex(primaryDofIdx, /*timeIdx=*/0);
                        constraintsMap_[globI] = constraints;
                        continue;
                    }
                }
            }
        }
    }

    // linearize the whole system
    void linearize_()
    {
        resetSystem_();

        // before the first iteration of each time step, we need to update the
        // constraints. (i.e., we assume that constraints can be time dependent, but they
        // can't depend on the solution.)
        if (model_().newtonMethod().numIterations() == 0)
            updateConstraintsMap_();

        applyConstraintsToSolution_();

        *matrix_ = 0.0;

        // storage to any exception that needs to be bridged out of the
        // parallel block below. initialized to null to indicate no exception
        std::exception_ptr exc_ptr = nullptr;

        // relinearize the elements...
        ThreadedEntityIterator<GridView, /*codim=*/0> threadedElemIt(gridView_());
#ifdef _OPENMP
#pragma omp parallel
#endif
        {
	        try {
		        ElementIterator elemIt = threadedElemIt.beginParallel();
		        ElementIterator nextElemIt = elemIt;
		        for (; !threadedElemIt.isFinished(elemIt); elemIt = nextElemIt) {
			        // give the model and the problem a chance to prefetch the data required
			        // to linearize the next element, but only if we need to consider it
			        nextElemIt = threadedElemIt.increment();
			        if (!threadedElemIt.isFinished(nextElemIt)) {
				        const auto& nextElem = *nextElemIt;
				        if (linearizeNonLocalElements
				            || nextElem.partitionType() == Dune::InteriorEntity)
				        {
					        model_().prefetch(nextElem);
					        problem_().prefetch(nextElem);
				        }
			        }

			        const Element& elem = *elemIt;
			        if (!linearizeNonLocalElements && elem.partitionType() != Dune::InteriorEntity)
				        continue;

			        linearizeElement_(elem);
		        }
	        }
	        // if an exception occurs in the parallel block, it won't escape the
	        // block; terminate() is called instead of a handler outside!
	        // hence, we tuck any exceptions that occurs away in the pointer if
	        // an exception occurs in more than one thread at the same time, we
	        // must pick one of them to be rethrown as we cannot have two active
	        // exceptions at the same time. this solution essentially picks one
	        // at random. this will be a problem if two different kinds of
	        // exceptions are thrown, for instance if one thread gets a
	        // numerical issue and the other one is out of memory
	        catch(...) {
#ifdef _OPENMP
		        std::lock_guard<std::mutex> take(this->exceptionLock);
#endif
		        exc_ptr = std::current_exception();
	        }
        }  // parallel block

        // after reduction from the parallel block, exc_ptr will point to
        // a valid exception if one occurred in one of the threads; rethrow
        // it here to let the outer handler take care of it properly
        if(exc_ptr) {
	        std::rethrow_exception(exc_ptr);
        }

        applyConstraintsToLinearization_();
    }

    // linearize an element in the interior of the process' grid partition
    void linearizeElement_(const Element& elem)
    {
        unsigned threadId = ThreadManager::threadId();

        ElementContext *elementCtx = elementCtx_[threadId];
        auto& localLinearizer = model_().localLinearizer(threadId);

        // the actual work of linearization is done by the local linearizer class
        localLinearizer.linearize(*elementCtx, elem);

        // update the right hand side and the Jacobian matrix
        if (GET_PROP_VALUE(TypeTag, UseLinearizationLock))
            globalMatrixMutex_.lock();

        size_t numPrimaryDof = elementCtx->numPrimaryDof(/*timeIdx=*/0);
        for (unsigned primaryDofIdx = 0; primaryDofIdx < numPrimaryDof; ++ primaryDofIdx) {
            unsigned globI = elementCtx->globalSpaceIndex(/*spaceIdx=*/primaryDofIdx, /*timeIdx=*/0);

            // update the right hand side
            residual_[globI] += localLinearizer.residual(primaryDofIdx);

            // update the global Jacobian matrix
            for (unsigned dofIdx = 0; dofIdx < elementCtx->numDof(/*timeIdx=*/0); ++ dofIdx) {
                unsigned globJ = elementCtx->globalSpaceIndex(/*spaceIdx=*/dofIdx, /*timeIdx=*/0);

                (*matrix_)[globJ][globI] += localLinearizer.jacobian(dofIdx, primaryDofIdx);
            }
        }

        if (GET_PROP_VALUE(TypeTag, UseLinearizationLock))
            globalMatrixMutex_.unlock();
    }

    // apply the constraints to the solution. (i.e., the solution of constraint degrees
    // of freedom is set to the value of the constraint.)
    void applyConstraintsToSolution_()
    {
        if (!enableConstraints_())
            return;

        // TODO: assuming a history size of 2 only works for Euler time discretizations!
        auto& sol = model_().solution(/*timeIdx=*/0);
        auto& oldSol = model_().solution(/*timeIdx=*/1);

        auto it = constraintsMap_.begin();
        const auto& endIt = constraintsMap_.end();
        for (; it != endIt; ++it) {
            sol[it->first] = it->second;
            oldSol[it->first] = it->second;
        }
    }

    // apply the constraints to the linearization. (i.e., for constrain degrees of
    // freedom the Jacobian matrix maps to identity and the residual is zero)
    void applyConstraintsToLinearization_()
    {
        if (!enableConstraints_())
            return;

        MatrixBlock idBlock = 0.0;
        for (unsigned i = 0; i < numEq; ++i)
            idBlock[i][i] = 1.0;

        auto it = constraintsMap_.begin();
        const auto& endIt = constraintsMap_.end();
        for (; it != endIt; ++it) {
            unsigned constraintDofIdx = it->first;

            // reset the column of the Jacobian matrix
            auto colIt = (*matrix_)[constraintDofIdx].begin();
            const auto& colEndIt = (*matrix_)[constraintDofIdx].end();
            for (; colIt != colEndIt; ++colIt)
                *colIt = 0.0;

            // put an identity matrix on the main diagonal of the Jacobian
            (*matrix_)[constraintDofIdx][constraintDofIdx] = idBlock;

            // make the right-hand side of constraint DOFs zero
            residual_[constraintDofIdx] = 0.0;
        }
    }

    static bool enableConstraints_()
    { return GET_PROP_VALUE(TypeTag, EnableConstraints); }

    Simulator *simulatorPtr_;
    std::vector<ElementContext*> elementCtx_;

    // The constraint equations (only non-empty if the
    // EnableConstraints property is true)
    std::map<unsigned, Constraints> constraintsMap_;

    // the jacobian matrix
    Matrix *matrix_;
    // the right-hand side
    GlobalEqVector residual_;


    std::mutex globalMatrixMutex_;
};

} // namespace Ewoms

#endif
