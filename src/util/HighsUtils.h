/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                                                                       */
/*    This file is part of the HiGHS linear optimization suite           */
/*                                                                       */
/*    Written and engineered 2008-2020 at the University of Edinburgh    */
/*                                                                       */
/*    Available as open-source under the MIT License                     */
/*                                                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/**@file util/HUtils.h
 * @brief Class-independent utilities for HiGHS
 * @author Julian Hall, Ivet Galabova, Qi Huangfu and Michael Feldmeier
 */
#ifndef UTIL_HIGHSUTILS_H_
#define UTIL_HIGHSUTILS_H_

#include <vector>
#include <string>
#include "HConfig.h"

#ifdef HiGHSDEV
struct HighsValueDistribution {
  int num_count_;
  int num_zero_;
  int num_one_;
  double min_value_;
  double max_value_;
  std::vector<double> limit_;
  std::vector<int> count_;
};
#endif
struct HighsScatterData {
  int max_num_point_;
  int num_point_;
  int last_point_;
  double linear_coeff0_;
  double linear_coeff1_;
  double log_coeff0_;
  double log_coeff1_;
  std::vector<double> value0_;
  std::vector<double> value1_;
  int num_error_comparison;
  int num_better_linear;
  int num_better_log;
};

double getNorm2(const std::vector<double> values);

/**
 * @brief Logical check of double being +Infinity
 */
bool highs_isInfinity(double val  //!< Value being tested against +Infinity
		      );
/**
 * @brief Returns the relative difference of two doubles
 */
double highs_relative_difference(
				 const double v0,
				 const double v1
				 );
#ifdef HiGHSDEV
/**
 * @brief Analyse the values of a vector, assessing how many are in
 * each power of ten, and possibly analyse the distribution of
 * different values
 */
void analyseVectorValues(
			 const char* message,               //!< Message to be printed
			 int vecDim,                        //!< Dimension of vector
			 const std::vector<double>& vec,    //!< Vector of values
			 bool analyseValueList = false,     //!< Possibly analyse the distribution of different
			                                    //!< values in the vector
			 std::string model_name = "Unknown" //!< Model name to report if analysing distribution of different
                                                            //!< values in the vector
			 );

void analyseMatrixSparsity(
			   const char* message,             //!< Message to be printed
			   int numCol,                      //!< Number of columns
			   int numRow,                      //!< Number of rows
			   const std::vector<int>& Astart,  //!< Matrix column starts
			   const std::vector<int>& Aindex   //!< Matrix row indices
			   );

bool initialiseValueDistribution(
				 const double min_value_limit,
				 const double max_value_limit,
				 const double base_value_limit,
				 HighsValueDistribution& value_distribution);

bool updateValueDistribution(
			     const double value,
			     HighsValueDistribution& value_distribution);

int integerPercentage(const int of, const int in);
double doublePercentage(const int of, const int in);

bool printValueDistribution(std::string value_name,
			    const HighsValueDistribution& value_distribution,
			    const int mu=0);


bool initialiseScatterData(const int max_num_point, HighsScatterData& scatter_data);
bool updateScatterData(const double value0, const double value1, HighsScatterData& scatter_data);
bool regressScatterData(HighsScatterData& scatter_data);
bool printScatterData(std::string name, const HighsScatterData& scatter_data);
void printScatterDataRegressionComparison(std::string name, const HighsScatterData& scatter_data);

#endif
#endif  // UTIL_HIGHSUTILS_H_
