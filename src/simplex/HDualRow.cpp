/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2020 at the University of Edinburgh    */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/**@file simplex/HDualRow.cpp
 * @brief
 * @author Julian Hall, Ivet Galabova, Qi Huangfu and Michael Feldmeier
 */
#include "simplex/HDualRow.h"

#include <cassert>
#include <iostream>

#include "lp_data/HConst.h"
#include "lp_data/HighsModelObject.h"
#include "simplex/HSimplex.h"
#include "simplex/HSimplexDebug.h"
#include "simplex/HVector.h"
#include "simplex/SimplexTimer.h"
#include "util/HighsSort.h"

using std::make_pair;
using std::pair;
using std::set;

void HDualRow::setupSlice(int size) {
  workSize = size;
  workMove = &workHMO.simplex_basis_.nonbasicMove_[0];
  workDual = &workHMO.simplex_info_.workDual_[0];
  workRange = &workHMO.simplex_info_.workRange_[0];
  work_devex_index = &workHMO.simplex_info_.devex_index_[0];

  // Allocate spaces
  packCount = 0;
  packIndex.resize(workSize);
  packValue.resize(workSize);

  workCount = 0;
  workData.resize(workSize);
  analysis = &workHMO.simplex_analysis_;
}

void HDualRow::setup() {
  // Setup common vectors
  const int numTot = workHMO.simplex_lp_.numCol_ + workHMO.simplex_lp_.numRow_;
  setupSlice(numTot);
  workNumTotPermutation = &workHMO.simplex_info_.numTotPermutation_[0];
  debug_zero_vector.assign(numTot, 0);

  // deleteFreelist() is being called in Phase 1 and Phase 2 since
  // it's in updatePivots(), but create_Freelist() is only called in
  // Phase 2. Hence freeList is not initialised when freeList.empty()
  // is used in deleteFreelist(), clear freeList now.
  freeList.clear();
}

void HDualRow::clear() {
  packCount = 0;
  workCount = 0;
}

void HDualRow::chooseMakepack(const HVector* row, const int offset) {
  /**
   * Pack the indices and values for the row
   *
   * Offset of numCol is used when packing row_ep
   */
  const int rowCount = row->count;
  const int* rowIndex = &row->index[0];
  const double* rowArray = &row->array[0];

  for (int i = 0; i < rowCount; i++) {
    const int index = rowIndex[i];
    const double value = rowArray[index];
    packIndex[packCount] = index + offset;
    packValue[packCount++] = value;
  }
}

void HDualRow::choosePossible() {
  /**
   * Determine the possible variables - candidates for CHUZC
   * TODO: Check with Qi what this is doing
   */
  const double Ta = workHMO.simplex_info_.update_count < 10
                        ? 1e-9
                        : workHMO.simplex_info_.update_count < 20 ? 3e-8 : 1e-6;
  const double Td = workHMO.scaled_solution_params_.dual_feasibility_tolerance;
  const int sourceOut = workDelta < 0 ? -1 : 1;
  workTheta = HIGHS_CONST_INF;
  workCount = 0;
  for (int i = 0; i < packCount; i++) {
    const int iCol = packIndex[i];
    const int move = workMove[iCol];
    const double alpha = packValue[i] * sourceOut * move;
    if (alpha > Ta) {
      workData[workCount++] = make_pair(iCol, alpha);
      const double relax = workDual[iCol] * move + Td;
      if (workTheta * alpha > relax) workTheta = relax / alpha;
    }
  }
}

void HDualRow::chooseJoinpack(const HDualRow* otherRow) {
  /**
   * Join pack of possible candidates in this row with possible
   * candidates in otherRow
   */
  const int otherCount = otherRow->workCount;
  const pair<int, double>* otherData = &otherRow->workData[0];
  copy(otherData, otherData + otherCount, &workData[workCount]);
  workCount = workCount + otherCount;
  workTheta = min(workTheta, otherRow->workTheta);
}

bool HDualRow::chooseFinal() {
  /**
   * Chooses the entering variable via BFRT and EXPAND
   *
   * It will
   * (1) reduce the candidates as a small collection
   * (2) choose by BFRT by going over break points
   * (3) choose final by alpha
   * (4) determine final flip variables
   */

  // 1. Reduce by large step BFRT
  analysis->simplexTimerStart(Chuzc2Clock);
  int fullCount = workCount;
  workCount = 0;
  double totalChange = 0;
  const double totalDelta = fabs(workDelta);
  double selectTheta = 10 * workTheta + 1e-7;
  for (;;) {
    for (int i = workCount; i < fullCount; i++) {
      int iCol = workData[i].first;
      double alpha = workData[i].second;
      double tight = workMove[iCol] * workDual[iCol];
      if (alpha * selectTheta >= tight) {
        swap(workData[workCount++], workData[i]);
        totalChange += workRange[iCol] * alpha;
      }
    }
    selectTheta *= 10;
    if (totalChange >= totalDelta || workCount == fullCount) break;
  }
  analysis->simplexTimerStop(Chuzc2Clock);

  // 2. Choose by small step BFRT

  original_workData = workData;
  alt_workCount = workCount;
  analysis->simplexTimerStart(Chuzc3Clock);
  analysis->simplexTimerStart(Chuzc3a0Clock);
  bool choose_ok = chooseFinalWorkGroupQuad();
  analysis->simplexTimerStop(Chuzc3a0Clock);
  if (!choose_ok) {
      analysis->simplexTimerStop(Chuzc3Clock);
      return true;
  }
  analysis->simplexTimerStart(Chuzc3a1Clock);
  chooseFinalWorkGroupHeap();
  analysis->simplexTimerStop(Chuzc3a1Clock);
  // 3. Choose large alpha
  analysis->simplexTimerStart(Chuzc3bClock);
  int breakIndex;
  int breakGroup;
  chooseFinalLargeAlpha(breakIndex, breakGroup, workData, workGroup);
  int alt_breakIndex;
  int alt_breakGroup;
  chooseFinalLargeAlpha(alt_breakIndex, alt_breakGroup, sorted_workData, alt_workGroup);
  analysis->simplexTimerStop(Chuzc3bClock);

  analysis->simplexTimerStart(Chuzc3cClock);

  int sourceOut = workDelta < 0 ? -1 : 1;
  workPivot = workData[breakIndex].first;
  workAlpha = workData[breakIndex].second * sourceOut * workMove[workPivot];
  if (workDual[workPivot] * workMove[workPivot] > 0) {
    workTheta = workDual[workPivot] / workAlpha;
  } else {
    workTheta = 0;
  }

  analysis->simplexTimerStop(Chuzc3cClock);

  int alt_workPivot = sorted_workData[alt_breakIndex].first;
  if (alt_workPivot != workPivot) {
    printf("Quad workPivot = %d; Heap workPivot = %d\n", workPivot, alt_workPivot);
    reportWorkDataAndGroup("Original", workCount, workData, workGroup);
    reportWorkDataAndGroup("Heap-derived", alt_workCount, sorted_workData,
			   alt_workGroup);
  }

  analysis->simplexTimerStart(Chuzc3dClock);

  // 4. Determine BFRT flip index: flip all
  fullCount = breakIndex;
  workCount = 0;
  for (int i = 0; i < workGroup[breakGroup]; i++) {
    const int iCol = workData[i].first;
    const int move = workMove[iCol];
    workData[workCount++] = make_pair(iCol, move * workRange[iCol]);
  }
  if (workTheta == 0) workCount = 0;
  analysis->simplexTimerStop(Chuzc3dClock);
  analysis->simplexTimerStart(Chuzc3eClock);
  sort(workData.begin(), workData.begin() + workCount);
  analysis->simplexTimerStop(Chuzc3eClock);
  analysis->simplexTimerStop(Chuzc3Clock);
  return false;
}

bool HDualRow::chooseFinalWorkGroupQuad() {
  const double Td = workHMO.scaled_solution_params_.dual_feasibility_tolerance;
  int fullCount = workCount;
  workCount = 0;
  double totalChange = 1e-12;
  double selectTheta = workTheta;
  const double totalDelta = fabs(workDelta);
  workGroup.clear();
  workGroup.push_back(0);
  const double iz_remainTheta = 1e100;
  int prev_workCount = workCount;
  double prev_remainTheta = iz_remainTheta;
  double prev_selectTheta = selectTheta;
  int debug_num_loop = 0;
  
  while (selectTheta < 1e18) {
    double remainTheta = iz_remainTheta;
    debug_num_loop++;
    int debug_loop_ln = 0;
    for (int i = workCount; i < fullCount; i++) {
      int iCol = workData[i].first;
      double value = workData[i].second;
      double dual = workMove[iCol] * workDual[iCol];
      // Tight satisfy
      if (dual <= selectTheta * value) {
        swap(workData[workCount++], workData[i]);
        totalChange += value * (workRange[iCol]);
      } else if (dual + Td < remainTheta * value) {
        remainTheta = (dual + Td) / value;
      }
      debug_loop_ln++;
    }
    workGroup.push_back(workCount);

    // Update selectTheta with the value of remainTheta;
    selectTheta = remainTheta;
    // Check for no change in this loop - to prevent infinite loop
    if ((workCount == prev_workCount) && (prev_selectTheta == selectTheta) &&
        (prev_remainTheta == remainTheta)) {
      debugDualChuzcFail(workHMO.options_, workCount, workData, workDual,
                         selectTheta, remainTheta);
      return false;
    }
    // Record the initial values of workCount, remainTheta and selectTheta for
    // the next pass through the loop - to check for infinite loop condition
    prev_workCount = workCount;
    prev_remainTheta = remainTheta;
    prev_selectTheta = selectTheta;
    if (totalChange >= totalDelta || workCount == fullCount) break;
  }
  return true;
}

bool HDualRow::chooseFinalWorkGroupHeap() {
  const double Td = workHMO.scaled_solution_params_.dual_feasibility_tolerance;
  int fullCount = alt_workCount;
  double totalChange = 1e-12;
  double selectTheta = workTheta;
  const double totalDelta = fabs(workDelta);
  int heap_num_en = 0;
  std::vector<int> heap_i;
  std::vector<double> heap_v;
  heap_i.resize(fullCount + 1);
  heap_v.resize(fullCount + 1);
  for (int i = 0; i < fullCount; i++) {
    int iCol = original_workData[i].first;
    double value = original_workData[i].second;
    double dual = workMove[iCol] * workDual[iCol];
    double ratio = dual / value;
    if (ratio < 1e18) {
      heap_num_en++;
      heap_i[heap_num_en] = i;
      heap_v[heap_num_en] = ratio;
    }
  }
  maxheapsort(&heap_v[0], &heap_i[0], heap_num_en);

  alt_workCount = 0;
  alt_workGroup.clear();
  alt_workGroup.push_back(alt_workCount);
  int this_group_first_entry = alt_workCount;
  sorted_workData.resize(heap_num_en);
  for (int en = 1; en <= heap_num_en; en++) {
    int i = heap_i[en];
    int iCol = original_workData[i].first;
    double value = original_workData[i].second;
    double dual = workMove[iCol] * workDual[iCol];
    if (dual > selectTheta * value) {
      // Breakpoint is in the next group, so record the pointer to its
      // first entry
      alt_workGroup.push_back(alt_workCount);
      this_group_first_entry = alt_workCount;
      selectTheta = (dual + Td) / value;
      // End loop if all permitted groups have been identified
      if (totalChange >= totalDelta) break;
    }
    // Store the breakpoint
    sorted_workData[alt_workCount].first = iCol;
    sorted_workData[alt_workCount].second = value;
    totalChange += value * (workRange[iCol]);
    alt_workCount++;
  }
  if (alt_workCount>this_group_first_entry)
    alt_workGroup.push_back(alt_workCount);
  return true;
}

void HDualRow::chooseFinalLargeAlpha(int& breakIndex, int& breakGroup,
			const std::vector<std::pair<int, double>>& workData,
				     const std::vector<int>& workGroup) {
  double finalCompare = 0;
  for (int i = 0; i < workCount; i++)
    finalCompare = max(finalCompare, workData[i].second);
  finalCompare = min(0.1 * finalCompare, 1.0);
  int countGroup = workGroup.size() - 1;
  breakGroup = -1;
  breakIndex = -1;
  for (int iGroup = countGroup - 1; iGroup >= 0; iGroup--) {
    double dMaxFinal = 0;
    int iMaxFinal = -1;
    for (int i = workGroup[iGroup]; i < workGroup[iGroup + 1]; i++) {
      if (dMaxFinal < workData[i].second) {
        dMaxFinal = workData[i].second;
        iMaxFinal = i;
      } else if (dMaxFinal == workData[i].second) {
        int jCol = workData[iMaxFinal].first;
        int iCol = workData[i].first;
        if (workNumTotPermutation[iCol] < workNumTotPermutation[jCol]) {
          iMaxFinal = i;
        }
      }
    }

    if (workData[iMaxFinal].second > finalCompare) {
      breakIndex = iMaxFinal;
      breakGroup = iGroup;
      break;
    }
  }

}

void HDualRow::reportWorkDataAndGroup(
    const std::string message, const int report_workCount,
    const std::vector<std::pair<int, double>>& report_workData,
    const std::vector<int>& report_workGroup) {
  const double Td = workHMO.scaled_solution_params_.dual_feasibility_tolerance;
  double totalChange = 1e-12;
  const double totalDelta = fabs(workDelta);
  printf("\n%s: totalDelta = %10.4g\nworkData\n  En iCol       Dual      Value      Ratio     Change\n",
         message.c_str(), totalDelta);
  for (int i = 0; i < report_workCount; i++) {
    int iCol = report_workData[i].first;
    double value = report_workData[i].second;
    double dual = workMove[iCol] * workDual[iCol];
    totalChange += value * (workRange[iCol]);
    printf("%4d %4d %10.4g %10.4g %10.4g %10.4g\n", i, iCol, dual, value, 
           dual / value, totalChange);
  }
  double selectTheta = workTheta;
  printf("workGroup\n  Ix:   selectTheta Entries\n");
  for (int group = 0; group < (int)report_workGroup.size() - 1; group++) {
    printf("%4d: selectTheta = %10.4g ", group, selectTheta);
    for (int en = report_workGroup[group]; en < report_workGroup[group + 1]; en++) {
      printf("%4d ", en);
    }
    printf("\n");
    int en = report_workGroup[group + 1];
    int iCol = original_workData[en].first;
    double value = original_workData[en].second;
    double dual = workMove[iCol] * workDual[iCol];
    selectTheta = (dual + Td) / value;
  }
}

bool HDualRow::compareWorkDataAndGroup() {
  bool no_difference = true;
  if (alt_workCount != workCount) {
    printf("Iteration %d: %d = alt_workCount != workCount = %d\n",
	   workHMO.iteration_counts_.simplex, alt_workCount, workCount);
    return false;
  }
 
  if ((int)alt_workGroup.size() != (int)workGroup.size()) {
    printf("Iteration %d: %d = alt_workGroup.size() != (int)workGroup.size() = %d\n",
	   workHMO.iteration_counts_.simplex, (int)alt_workGroup.size(), (int)workGroup.size());
    return false;
  }
  if (workGroup[0] != alt_workGroup[0]) {
    printf("Group workGroup[0] = %4d != %4d = alt_workGroup[0]\n",
	   workGroup[0], alt_workGroup[0]);
    return false;
  }
  for (int group = 0; group < (int)workGroup.size() - 1; group++) {
    if (workGroup[group+1] != alt_workGroup[group+1]) {
      printf("Group workGroup[%4d] = %4d != %4d = alt_workGroup[%4d]\n",
	     group+1, workGroup[group+1], alt_workGroup[group+1], group+1);
      return false;
    }
    for (int en = workGroup[group]; en < workGroup[group+1]; en++)
      debug_zero_vector[workData[en].first] = 1;
    for (int en = alt_workGroup[group]; en < alt_workGroup[group+1]; en++) {
      int iCol = sorted_workData[en].first;
      if (debug_zero_vector[iCol] != 1) {
	no_difference = false;
	printf("workGroup[%4d] does not contain column %d\n", group, iCol);
      }
      debug_zero_vector[iCol] = 0;
    }
    for (int en = workGroup[group]; en < workGroup[group+1]; en++) {
      int iCol = workData[en].first;
      if (debug_zero_vector[iCol] == 1) {
	no_difference = false;
	printf("alt_workGroup[%4d] does not contain column %d\n", group, iCol);
      }
      debug_zero_vector[iCol] = 0;
    }
    for (int iCol = 0; iCol < (int)debug_zero_vector.size(); iCol++) 
      assert(debug_zero_vector[iCol] == 0);
  }
  if (!no_difference) printf("WorkDataAndGroup difference in Iteration %d\n",
			     workHMO.iteration_counts_.simplex);
    
  return no_difference;
}

void HDualRow::updateFlip(HVector* bfrtColumn) {
  double* workDual = &workHMO.simplex_info_.workDual_[0];
  double dual_objective_value_change = 0;
  bfrtColumn->clear();
  for (int i = 0; i < workCount; i++) {
    const int iCol = workData[i].first;
    const double change = workData[i].second;
    double local_dual_objective_change = change * workDual[iCol];
    local_dual_objective_change *= workHMO.scale_.cost_;
    dual_objective_value_change += local_dual_objective_change;
    flip_bound(workHMO, iCol);
    workHMO.matrix_.collect_aj(*bfrtColumn, iCol, change);
  }
  workHMO.simplex_info_.updated_dual_objective_value +=
      dual_objective_value_change;
}

void HDualRow::updateDual(double theta) {
  analysis->simplexTimerStart(UpdateDualClock);
  double* workDual = &workHMO.simplex_info_.workDual_[0];
  double dual_objective_value_change = 0;
  for (int i = 0; i < packCount; i++) {
    workDual[packIndex[i]] -= theta * packValue[i];
    // Identify the change to the dual objective
    int iCol = packIndex[i];
    const double delta_dual = theta * packValue[i];
    const double local_value = workHMO.simplex_info_.workValue_[iCol];
    double local_dual_objective_change =
        workHMO.simplex_basis_.nonbasicFlag_[iCol] *
        (-local_value * delta_dual);
    local_dual_objective_change *= workHMO.scale_.cost_;
    dual_objective_value_change += local_dual_objective_change;
  }
  workHMO.simplex_info_.updated_dual_objective_value +=
      dual_objective_value_change;
  analysis->simplexTimerStop(UpdateDualClock);
}

void HDualRow::createFreelist() {
  freeList.clear();
  for (int i = 0; i < workHMO.simplex_lp_.numCol_ + workHMO.simplex_lp_.numRow_;
       i++) {
    if (workHMO.simplex_basis_.nonbasicFlag_[i] &&
        highs_isInfinity(-workHMO.simplex_info_.workLower_[i]) &&
        highs_isInfinity(workHMO.simplex_info_.workUpper_[i]))
      freeList.insert(i);
  }
  debugFreeListNumEntries(workHMO, freeList);
}

void HDualRow::createFreemove(HVector* row_ep) {
  // TODO: Check with Qi what this is doing and why it's expensive
  if (!freeList.empty()) {
    double Ta = workHMO.simplex_info_.update_count < 10
                    ? 1e-9
                    : workHMO.simplex_info_.update_count < 20 ? 3e-8 : 1e-6;
    int sourceOut = workDelta < 0 ? -1 : 1;
    set<int>::iterator sit;
    for (sit = freeList.begin(); sit != freeList.end(); sit++) {
      int iCol = *sit;
      assert(iCol < workHMO.simplex_lp_.numCol_);
      double alpha = workHMO.matrix_.compute_dot(*row_ep, iCol);
      if (fabs(alpha) > Ta) {
        if (alpha * sourceOut > 0)
          workHMO.simplex_basis_.nonbasicMove_[iCol] = 1;
        else
          workHMO.simplex_basis_.nonbasicMove_[iCol] = -1;
      }
    }
  }
}
void HDualRow::deleteFreemove() {
  if (!freeList.empty()) {
    set<int>::iterator sit;
    for (sit = freeList.begin(); sit != freeList.end(); sit++) {
      int iCol = *sit;
      assert(iCol < workHMO.simplex_lp_.numCol_);
      workHMO.simplex_basis_.nonbasicMove_[iCol] = 0;
    }
  }
}

void HDualRow::deleteFreelist(int iColumn) {
  if (!freeList.empty()) {
    if (freeList.count(iColumn)) freeList.erase(iColumn);
  }
}

void HDualRow::computeDevexWeight(const int slice) {
  const bool rp_computed_edge_weight = false;
  computed_edge_weight = 0;
  for (int el_n = 0; el_n < packCount; el_n++) {
    int vr_n = packIndex[el_n];
    if (!workHMO.simplex_basis_.nonbasicFlag_[vr_n]) {
      //      printf("Basic variable %d in packIndex is skipped\n", vr_n);
      continue;
    }
    double pv = work_devex_index[vr_n] * packValue[el_n];
    if (pv) {
      computed_edge_weight += pv * pv;
    }
  }
  if (rp_computed_edge_weight) {
    if (slice >= 0)
      printf(
          "HDualRow::computeDevexWeight: Slice %1d; computed_edge_weight = "
          "%11.4g\n",
          slice, computed_edge_weight);
  }
}
