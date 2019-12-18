#include <RcppArmadillo.h>
#include <memory>
#include "solvers.h"
#include "penalties.h"
#include "families.h"
#include "screening_rules.h"

using namespace Rcpp;
using namespace arma;

template <typename T>
List
owlCpp(const T& x, const mat& y, const List control)
{
  auto p = x.n_cols;
  auto n = x.n_rows;
  auto m = as<uword>(control["n_targets"]);

  // parameter packs for penalty and solver
  auto penalty_args = as<List>(control["penalty"]);

  auto tol_dev_ratio = as<double>(control["tol_dev_ratio"]);
  auto tol_dev_change = as<double>(control["tol_dev_change"]);
  auto max_variables = as<uword>(control["max_variables"]);

  auto diagnostics = as<bool>(control["diagnostics"]);
  auto verbosity = as<int>(control["verbosity"]);

  // solver arguments
  auto max_passes = as<uword>(control["max_passes"]);
  auto tol_rel_gap = as<double>(control["tol_rel_gap"]);
  auto tol_infeas = as<double>(control["tol_infeas"]);

  auto family_args = as<List>(control["family"]);
  auto fit_intercept = as<bool>(control["fit_intercept"]);
  auto screening = as<bool>(control["screening"]);
  auto sigma_type = as<std::string>(control["sigma_type"]);

  auto standardize_features = as<bool>(control["standardize_features"]);
  auto is_sparse = as<bool>(control["is_sparse"]);
  auto n_sigma = as<uword>(control["n_sigma"]);

  auto lambda = as<vec>(control["lambda"]);
  auto sigma  = as<vec>(control["sigma"]);

  // get scaled vector of feature matrix centers for use in sparse fitting
  vec x_center        = as<vec>(control["x_center"]);
  vec x_scale         = as<vec>(control["x_scale"]);
  vec x_scaled_center = x_center/x_scale;

  auto family_choice = as<std::string>(family_args["name"]);

  // setup family and penalty
  if (verbosity >= 1)
    Rcpp::Rcout << "setting up family" << std::endl;

  auto family = setupFamily(family_choice);

  if (verbosity >= 1)
    Rcpp::Rcout << "setting up penalty" << std::endl;

  auto penalty = setupPenalty(penalty_args);

  cube betas(p, m, n_sigma, fill::zeros);
  cube intercepts(1, m, n_sigma);

  rowvec intercept(m, fill::zeros);
  mat beta(p, m, fill::zeros);

  uword n_variables = static_cast<uword>(fit_intercept);

  if (verbosity >= 1)
    Rcpp::Rcout << "fitting intercept for null model" << std::endl;

  if (fit_intercept)
    intercept = family->fitNullModel(y, m);

  mat linear_predictor = linearPredictor(x,
                                         beta,
                                         intercept,
                                         x_center,
                                         x_scale,
                                         standardize_features);

  double null_deviance = 2*family->primal(y, linear_predictor);
  std::vector<double> deviances;
  std::vector<double> deviance_ratios;

  rowvec intercept_prev(m, fill::zeros);
  mat beta_prev(p, m, fill::zeros);

  uvec passes(n_sigma);
  std::vector<std::vector<double>> primals;
  std::vector<std::vector<double>> duals;
  std::vector<std::vector<double>> timings;
  std::vector<std::vector<double>> infeasibilities;
  std::vector<std::vector<unsigned>> line_searches;
  uvec violations(n_sigma, fill::zeros);

  mat linear_predictor_prev(n, m);
  mat gradient_prev(p, m);
  mat pseudo_gradient_prev(n, m);

  // sets of active predictors
  field<uvec> active_sets(n_sigma);
  uvec active_set;

  if (verbosity >= 1)
    Rcpp::Rcout << "setting up solver" << std::endl;

  auto solver = setupSolver("fista",
                            standardize_features,
                            is_sparse,
                            diagnostics,
                            max_passes,
                            tol_rel_gap,
                            tol_infeas,
                            verbosity);

  Results res;

  uword k = 0;

  while (k < n_sigma) {

    if (verbosity >= 1)
      Rcpp::Rcout << "penalty: " << k + 1 << std::endl;

    if (!screening) {

      active_set = regspace<uvec>(0, p-1);

    } else {

      if (sigma_type == "sequence" && k == 0) {
        // no predictors active at first step (except intercept)

        active_set.set_size(0);
        beta.zeros();

      } else {

        // NOTE(JL): the screening rules should probably not be used if
        // the coefficients from the previous fit are already very dense

        linear_predictor_prev = linearPredictor(x,
                                                beta_prev,
                                                intercept_prev,
                                                x_center,
                                                x_scale,
                                                standardize_features);

        pseudo_gradient_prev = family->pseudoGradient(y, linear_predictor_prev);
        gradient_prev = x.t() * pseudo_gradient_prev;

        active_set = activeSet(y,
                               gradient_prev,
                               lambda*sigma(k),
                               lambda*sigma(k-1));
      }
    }

    if (active_set.n_elem == 0) {
      // null (intercept only) model

      if (fit_intercept)
        intercept = family->fitNullModel(y, m);

      beta.zeros();
      passes(k) = 0;

      if (diagnostics) {
        primals.emplace_back(0);
        duals.emplace_back(0);
        infeasibilities.emplace_back(0);
        timings.emplace_back(0);
        line_searches.emplace_back(0);
      }

    } else if (active_set.n_elem == p) {
      // all features active

      res = solver->fit(x,
                        y,
                        family,
                        penalty,
                        intercept,
                        beta,
                        fit_intercept,
                        lambda*sigma(k),
                        x_center,
                        x_scale);

      passes(k) = res.passes;
      beta = res.beta;
      intercept = res.intercept;

      if (diagnostics) {
        primals.push_back(res.primals);
        duals.push_back(res.duals);
        infeasibilities.push_back(res.infeasibilities);
        timings.push_back(res.time);
        line_searches.emplace_back(res.line_searches);
      }

    } else {

      bool kkt_violation = true;

      do {
        if (verbosity > 0) {
          Rcpp::Rcout << "\t active set: " << std::endl;
          active_set.print();
          Rcpp::Rcout << std::endl;
        }

        T x_subset = matrixSubset(x, active_set);

        res = solver->fit(x_subset,
                          y,
                          family,
                          penalty,
                          intercept,
                          beta.rows(active_set),
                          fit_intercept,
                          lambda.head(active_set.n_elem*m)*sigma(k),
                          x_center(active_set),
                          x_scale(active_set));

        beta.rows(active_set) = res.beta;
        intercept = res.intercept;
        passes(k) = res.passes;

        linear_predictor_prev = linearPredictor(x,
                                                beta,
                                                intercept,
                                                x_center,
                                                x_scale,
                                                standardize_features);

        pseudo_gradient_prev = family->pseudoGradient(y, linear_predictor_prev);
        gradient_prev = x.t() * pseudo_gradient_prev;

        uvec possible_failures =
          penalty->kktCheck(gradient_prev, beta, lambda*sigma(k), 1e-6);
        uvec check_failures = setDiff(possible_failures, active_set);

        if (verbosity >= 1) {
          Rcpp::Rcout << "\t kkt-failures at: ";
          check_failures.print();
          Rcpp::Rcout << std::endl;
        }

        kkt_violation = check_failures.n_elem > 0;
        violations(k) += check_failures.n_elem;

        active_set = setUnion(check_failures, active_set);

        checkUserInterrupt();

      } while (kkt_violation);

      if (diagnostics) {
        primals.push_back(res.primals);
        duals.push_back(res.duals);
        infeasibilities.push_back(res.infeasibilities);
        timings.push_back(res.time);
        line_searches.push_back(res.line_searches);
      }
    }

    // store coefficients and intercept
    double deviance = res.deviance;
    double deviance_ratio = 1.0 - deviance/null_deviance;

    deviances.push_back(deviance);
    deviance_ratios.push_back(deviance_ratio);
    betas.slice(k) = beta;
    intercepts.slice(k) = intercept;
    intercept_prev = intercept;
    beta_prev = beta;

    active_sets(k) = active_set;

    if (verbosity >= 1)
      Rcout << "deviance: " << deviance << "\t"
            << "deviance ratio: " << deviance_ratio << std::endl;

    if (k > 0) {
      // stop path if fractional deviance change is small
      double deviance_change =
        std::abs((deviances[k-1] - deviances[k])/deviances[k-1]);

      if (verbosity >= 1)
        Rcout << "deviance change: " << deviance_change << std::endl;

      if (deviance_change < tol_dev_change || deviance_ratio > tol_dev_ratio) {
        k++;
        break;
      }
    }

    n_variables = static_cast<uword>(fit_intercept) + accu(any(beta != 0, 1));

    if (verbosity >= 1)
      Rcout << "number of variables: " << n_variables << std::endl;

    if (n_variables > max_variables) {
      break;
    }

    k++;

    checkUserInterrupt();
  }

  intercepts.set_size(1, m, k);
  betas.set_size(p, m, k);
  active_sets.set_size(k);
  violations.set_size(k);

  return List::create(
    Named("intercept")           = wrap(intercepts),
    Named("beta")                = wrap(betas),
    Named("active_sets")         = wrap(active_sets),
    Named("passes")              = wrap(passes),
    Named("primals")             = wrap(primals),
    Named("duals")               = wrap(duals),
    Named("infeasibilities")     = wrap(infeasibilities),
    Named("time")                = wrap(timings),
    Named("line_searches")       = wrap(line_searches),
    Named("violations")          = wrap(violations),
    Named("deviance_ratio")      = wrap(deviance_ratios),
    Named("null_deviance")       = wrap(null_deviance),
    Named("path_length")         = k
  );
}


// [[Rcpp::export]]
Rcpp::List
owlSparse(const arma::sp_mat& x,
          const arma::mat& y,
          const Rcpp::List control)
{
  return owlCpp(x, y, control);
}

// [[Rcpp::export]]
Rcpp::List
owlDense(const arma::mat& x,
         const arma::mat& y,
         const Rcpp::List control)
{
  return owlCpp(x, y, control);
}
