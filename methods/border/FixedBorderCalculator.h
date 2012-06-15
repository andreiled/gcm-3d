#ifndef _GCM_FIXED_BORDER_CALCULATOR_H
#define _GCM_FIXED_BORDER_CALCULATOR_H  1

#include <gsl/gsl_linalg.h>
#include <vector>
using std::vector;

#include "../../datatypes/ElasticNode.h"
#include "../../datatypes/ElasticMatrix3D.h"
#include "../../datatypes/Basis.h"

class FixedBorderCalculator
{
public:
	FixedBorderCalculator();
	~FixedBorderCalculator();
	void do_calc(ElasticNode* new_node, basis* basis, ElasticMatrix3D* matrix, float* values[], bool inner[], int stage);
private:
	// Used for border calculation
	gsl_matrix *U_gsl;
	gsl_vector *om_gsl;
	gsl_vector *x_gsl;
	gsl_permutation *p_gsl;
};

#endif