#include "postgres.h"

#include "dynamic_ext.h"
#include "utils/selfuncs.h"

/*
 * Estimate cost of index scan.
 */
void decostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
                    Cost *indexStartupCost, Cost *indexTotalCost,
                    Selectivity *indexSelectivity, double *indexCorrelation,
                    double *indexPages) {
  GenericCosts costs = {0};
  IndexOptInfo *index = path->indexinfo;

  costs.numIndexTuples = index->tuples;

  genericcostestimate(root, path, loop_count, &costs);

  *indexStartupCost = costs.indexStartupCost;
  *indexTotalCost = costs.indexTotalCost;
  *indexSelectivity = costs.indexSelectivity;
  *indexCorrelation = costs.indexCorrelation;
  *indexPages = costs.numIndexPages;
}
