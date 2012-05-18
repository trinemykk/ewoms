// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*****************************************************************************
 *   Copyright (C) 2010 by Katherina Baber, Klaus Mosthaf                    *
 *   Copyright (C) 2008-2009 by Bernd Flemisch, Andreas Lauser               *
 *   Institute for Modelling Hydraulic and Environmental Systems             *
 *   University of Stuttgart, Germany                                        *
 *   email: <givenname>.<name>@iws.uni-stuttgart.de                          *
 *                                                                           *
 *   This program is free software: you can redistribute it and/or modify    *
 *   it under the terms of the GNU General Public License as published by    *
 *   the Free Software Foundation, either version 2 of the License, or       *
 *   (at your option) any later version.                                     *
 *                                                                           *
 *   This program is distributed in the hope that it will be useful,         *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *   GNU General Public License for more details.                            *
 *                                                                           *
 *   You should have received a copy of the GNU General Public License       *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.   *
 *****************************************************************************/
/*!
 * \file
 *
 * \brief Contains the quantities which are constant within a
 *        finite volume in the Stokes box model.
 */
#ifndef DUMUX_STOKES_VOLUME_VARIABLES_HH
#define DUMUX_STOKES_VOLUME_VARIABLES_HH

#include "stokesproperties.hh"

#include <dumux/boxmodels/common/boxvolumevariables.hh>
#include <dumux/material/fluidstates/immisciblefluidstate.hh>

#include<dune/common/version.hh>
#if DUNE_VERSION_NEWER_REV(DUNE_COMMON, 2,2,0)
// dune 2.2
#include<dune/geometry/quadraturerules.hh>
#else
// dune 2.1
#include<dune/grid/common/quadraturerules.hh>
#endif

#include <dune/common/fvector.hh>

namespace Dumux
{

/*!
 * \ingroup BoxStokesModel
 * \ingroup BoxVolumeVariables
 * \brief Contains the quantities which are are constant within a
 *        finite volume in the Stokes box model.
 */
template <class TypeTag>
class StokesVolumeVariables : public BoxVolumeVariables<TypeTag>
{
    typedef BoxVolumeVariables<TypeTag> ParentType;
    typedef typename GET_PROP_TYPE(TypeTag, Scalar) Scalar;
    typedef typename GET_PROP_TYPE(TypeTag, GridView) GridView;
    typedef typename GET_PROP_TYPE(TypeTag, VolumeVariables) Implementation;
    typedef typename GET_PROP_TYPE(TypeTag, Indices) Indices;
    typedef typename GET_PROP_TYPE(TypeTag, FluidSystem) FluidSystem;
    typedef typename GET_PROP_TYPE(TypeTag, FluidState) FluidState;
    typedef typename GET_PROP_TYPE(TypeTag, ElementContext) ElementContext;

    enum { numComponents = FluidSystem::numComponents };
    enum { dim = GridView::dimension };
    enum { dimWorld = GridView::dimensionworld };
    enum { pressureIdx = Indices::pressureIdx };
    enum { moleFrac1Idx = Indices::moleFrac1Idx };
    enum { phaseIdx = GET_PROP_VALUE(TypeTag, StokesPhaseIndex) };

    typedef typename GridView::ctype CoordScalar;
    typedef Dune::FieldVector<Scalar, dimWorld> DimVector;
    typedef Dune::FieldVector<CoordScalar, dim> LocalPosition;

public:
    /*!
     * \copydoc BoxVolumeVariables::update()
     */
    void update(const ElementContext &elemCtx, int scvIdx, int timeIdx)
    {
        ParentType::update(elemCtx, scvIdx, timeIdx);

        asImp_().updateTemperature_(elemCtx, scvIdx, timeIdx);

        const auto &priVars = elemCtx.primaryVars(scvIdx, timeIdx);
        fluidState_.setPressure(phaseIdx, priVars[pressureIdx]);

        // set the phase composition
        Scalar sumx = 0;
        for (int compIdx = 1; compIdx < numComponents; ++compIdx) {
            fluidState_.setMoleFraction(phaseIdx, compIdx, priVars[moleFrac1Idx + compIdx - 1]);
            sumx += priVars[moleFrac1Idx + compIdx - 1];
        }
        fluidState_.setMoleFraction(phaseIdx, 0, 1 - sumx);
                
        // create NullParameterCache and do dummy update
        typename FluidSystem::ParameterCache paramCache;
        paramCache.updateAll(fluidState_);

        fluidState_.setDensity(phaseIdx,
                               FluidSystem::density(fluidState_,
                                                    paramCache,
                                                    phaseIdx));
        fluidState_.setViscosity(phaseIdx,
                                 FluidSystem::viscosity(fluidState_,
                                                        paramCache,
                                                        phaseIdx));
        
        // compute and set the energy related quantities
        asImp_().updateEnergy_(paramCache, elemCtx, scvIdx, timeIdx);

        // the effective velocity of the control volume
        for (int dimIdx = 0; dimIdx < dimWorld; ++dimIdx)
            velocityCenter_[dimIdx] = priVars[Indices::velocity0Idx + dimIdx];

        // the gravitational acceleration applying to the material
        // inside the volume
        gravity_ = elemCtx.problem().gravity();
    }

    /*!
     * \brief Update the gradients for the sub-control volumes.
     */
    void updateScvGradients(const ElementContext &elemCtx, int scvIdx, int timeIdx)
    {
        // calculate the pressure gradient at the SCV using finite
        // element gradients
        pressureGrad_ = 0.0;
        for (int i = 0; i < elemCtx.numScv(); ++i) {
            const auto &feGrad = elemCtx.fvElemGeom(timeIdx).subContVol[scvIdx].gradCenter[i];
            DimVector tmp(feGrad);
            tmp *= elemCtx.volVars(i, timeIdx).fluidState().pressure(phaseIdx);
            
            pressureGrad_ += tmp;
        }            

        // integrate the velocity over the sub-control volume
        //const auto &elemGeom = elemCtx.element().geometry();
        const auto &fvElemGeom = elemCtx.fvElemGeom(timeIdx);
        const auto &scvLocalGeom = *fvElemGeom.subContVol[scvIdx].localGeometry;
        
        Dune::GeometryType geomType = scvLocalGeom.type();
        static const int quadratureOrder = 2;
        const auto &rule = Dune::QuadratureRules<Scalar,dimWorld>::rule(geomType, quadratureOrder);
        
        // integrate the veloc over the sub-control volume
        velocity_ = 0.0;
        for (auto it = rule.begin(); it != rule.end(); ++ it)
        {
            const auto &posScvLocal = it->position();
            const auto &posElemLocal = scvLocalGeom.global(posScvLocal);
            
            DimVector velocityAtPos = velocityAtPos_(elemCtx, timeIdx, posElemLocal);
            Scalar weight = it->weight();
            Scalar detjac = 1.0;
            //scvLocalGeom.integrationElement(posScvLocal) *
            //elemGeom.integrationElement(posElemLocal);
            velocity_.axpy(weight * detjac,  velocityAtPos);
        }

        // since we want the average velocity, we have to divide the
        // integrated value by the volume of the SCV
        //velocity_ /= fvElemGeom.subContVol[scvIdx].volume;
    }


    /*!
     * \brief Returns the phase state for the control-volume.
     */
    const FluidState &fluidState() const
    { return fluidState_; }
    
    /*!
     * \brief Returns the molar density \f$\mathrm{[mol/m^3]}\f$ of
     *        the fluid within the sub-control volume.
     */
    Scalar molarDensity() const
    { return fluidState_.density(phaseIdx) / fluidState_.averageMolarMass(phaseIdx); }

    /*!
     * \brief Returns the average velocity in the sub-control volume.
     */
    const DimVector &velocity() const
    { return velocity_; }

    /*!
     * \brief Returns the velocity at the center in the sub-control volume.
     */
    const DimVector &velocityCenter() const
    { return velocityCenter_; }

    /*!
     * \brief Returns the pressure gradient in the sub-control volume.
     */
    const DimVector &pressureGradient() const
    { return pressureGrad_; }

    /*!
     * \brief Returns the gravitational acceleration vector in the
     *        sub-control volume.
     */
    const DimVector &gravity() const
    { return gravity_; } 

protected:
    DimVector velocityAtPos_(const ElementContext elemCtx,
                          int timeIdx,
                          const LocalPosition &localPos) const
    {
        const auto &localFiniteElement = 
            elemCtx.fvElemGeom(timeIdx).localFiniteElement();

        typedef Dune::FieldVector<Scalar, 1> ShapeValue;
        std::vector<ShapeValue> shapeValue;
        
        localFiniteElement.localBasis().evaluateFunction(localPos, shapeValue);

        DimVector result(0.0);
        for (int scvIdx = 0; scvIdx < elemCtx.numScv(); scvIdx++) {
            result.axpy(shapeValue[scvIdx][0], elemCtx.volVars(scvIdx, timeIdx).velocityCenter());
        }

        return result;
    }

    template<class ParameterCache>
    void updateEnergy_(const ParameterCache &paramCache,
                       const ElementContext &elemCtx,
                       int scvIdx, int timeIdx)
    { }

    void updateTemperature_(const ElementContext &elemCtx,
                            int scvIdx, int timeIdx)
    {
        Scalar T = elemCtx.problem().temperature(elemCtx, scvIdx, timeIdx);
        this->fluidState_.setTemperature(T);
    }

    DimVector velocity_;
    DimVector velocityCenter_;
    DimVector gravity_;
    DimVector pressureGrad_;
    FluidState fluidState_;

private:
    Implementation &asImp_()
    { return *static_cast<Implementation*>(this); }
    const Implementation &asImp_() const
    { return *static_cast<const Implementation*>(this); }
};

}

#endif
