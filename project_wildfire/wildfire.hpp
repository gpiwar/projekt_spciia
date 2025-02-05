// SPDX-FileCopyrightText: 2015 - 2024 Marcin Łoś <marcin.los.91@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef FIRE_FIRE_HPP
#define FIRE_FIRE_HPP

#include <cmath>

#include "ads/executor/galois.hpp"
#include "ads/output_manager.hpp"
#include "ads/simulation.hpp"
#include "ads/util.hpp"


#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>

#define FUEL_MATRIX_CSV "fuel_matrix.csv"


std::string find_file(const std::string &aFileName, const std::string &aLookupStartDir = "/home")
{
    std::string aFilePath {""};
    for(const auto& entry : std::filesystem::recursive_directory_iterator(aLookupStartDir))
    {
        if(entry.is_regular_file() && entry.path().filename() == aFileName)
        {
            aFilePath = entry.path().string();
            break;
        }
    }
    return aFilePath;
}

std::vector<std::vector<int>> load_fuel_matrix(const std::string& file_path) {
    std::ifstream file(file_path);
    std::vector<std::vector<int>> fuel_matrix;
    std::string line;

    while (getline(file, line)) {
        std::istringstream iss(line);
        std::vector<int> row;
        std::string val;
        
        while (getline(iss, val, ',')) {
            row.push_back(std::stoi(val));
        }
        
        fuel_matrix.push_back(row);
    }

    return fuel_matrix;
}

inline double custom_wind(double x, double t) {
    // Zmienność wiatru w kierunku x zależna od czasu i przestrzeni
    return 10 * sin(t / 10.0 * 2 * M_PI) * sin(x / 10.0 * 2 * M_PI);
}


inline double falloff(double r, double R, double t) {
    if (t < r)
        return 1.0;
    if (t > R)
        return 0.0;
    double h = (t - r) / (R - r);
    return std::pow((h - 1) * (h + 1), 2);
}

inline double bump(double r, double R, double x, double y) {
    double dx = x - 2;
    double dy = y - 58;
    double t = std::sqrt(dx * dx + dy * dy) / 100;
    return falloff(r / 200, R / 200, t);
}

namespace ads::problems {

class fire : public simulation_2d {
private:
    using Base = simulation_2d;
    vector_type u, u_prev;
    vector_type fuel, fuel_prev;
    vector_type wind, wind_prev;
    // double C1 = 1;
    // double C2 = 1;
    // double C3 = 1;
    // double C4 = 1;
    // double C5 = 1;
    // double C6 = 1;
    // double T0 = 1;
    // double bx = 40;
    // double by = 20;
    double bx = 0;
    double by = 0;

    double ch = 1.0;
    double Ar = 5.7e-5;
    double rho = 1.293;
    double cp = 1.0;
    double cw = 0.5;
    double kappa = 0.3;
    double xi = 2e-2;
    double sigma = 5.67e-8;
    double eps = 0.05; // "percentage error"
    double delta_x = 3.5e-2 / eps;
    double delta_z = 1.5 * eps;
    double T0 = 300;
    double Tcomb = 1200;
    double Tig = 800;
    double Ta = 300; // ???

    double M = 2;
    double M1 = 1;

    galois_executor executor{4};
    output_manager<2> output;

public:
    explicit fire(const config_2d& config)
    : Base{config}
    , u{shape()}
    , u_prev{shape()}
    , fuel{shape()}
    , fuel_prev{shape()}
    , wind{shape()}
    , wind_prev{shape()}
    , output{x.B, y.B, 300} { }

    double init_state(double x, double y) {
        double r = 5;
        double R = 5;
        return T0 + Tcomb * bump(r, R, x, y);
    };

private:
    void before() override {
        prepare_matrices();
        
        // Load the binary matrix representing fuel areas
        std::string fuel_matrix_path = find_file(FUEL_MATRIX_CSV);
        std::vector<std::vector<int>> fuel_matrix = load_fuel_matrix(fuel_matrix_path);

        auto init = [this](double x, double y) { return init_state(x, y); };
        projection(u, init);
        solve(u);
        output.to_file(u, "out_0.data");

        auto fuel_init = [&fuel_matrix](double x, double y) { 
        int ix = static_cast<int>(x); // Convert x and y to indices
        int iy = static_cast<int>(y);
        return fuel_matrix[iy][ix];
        };
        projection(fuel, fuel_init);
        solve(fuel);
        output.to_file(fuel, "fuel_0.data");
        
        auto wind_init = [](double x, double y) { return   			  
        custom_wind(x, 0.0); };
	projection(wind, wind_init);
	solve(wind);
	output.to_file(wind, "wind_0.data");
    	};

    void before_step(int /*iter*/, double /*t*/) override {
        using std::swap;
        swap(u, u_prev);
        swap(fuel, fuel_prev);
        swap(wind, wind_prev);
    }

    void step(int /*iter*/, double t) override {
        compute_rhs(t);
        solve(u);
        solve(fuel);
        solve(wind);
    }

void compute_rhs(double t) {
    auto& rhs = u;
    auto& rhs_fuel = fuel;
    auto& rhs_wind = wind;

    zero(rhs);
    zero(rhs_fuel);
    zero(rhs_wind);
    executor.for_each(elements(), [&](index_type e) {
        auto U = element_rhs();
        auto F = element_rhs();
        auto W = element_rhs();

        double J = jacobian(e);
        for (auto q : quad_points()) {
            double w = weight(q);
            auto x = point(e, q);

            // wind
            double bx = custom_wind(x[0], t);
            double by = custom_wind(x[1], t);
            
            //wind_prev(e, q) = std::sqrt(bx * bx + by * by); 
            
            value_type u = eval_fun(u_prev, e, q);
            value_type fuel = eval_fun(fuel_prev, e, q);
	    value_type wind = eval_fun(wind_prev, e, q);

            for (auto a : dofs_on_element(e)) {
                auto aa = dof_global_to_local(e, a);
                value_type v = eval_basis(e, q, a);
                double inv = 1.0 / (rho * cp);

                double delta = u.val > Tig && fuel.val > 0.2 ? 1.0 : 0.0;
                double hc = -70; // enthalpy
                double r = delta * Ar * u.val * std::exp(-Ta / u.val) ;
                double Rc = - 1e4 * rho * ch * hc * M / M1 * r;
                double wind = (bx * u.dx + by * u.dy);
                // double wind = (bx + by); for testing
                double Qw = - rho * cw * wind;
                
                double qc = - kappa * grad_dot(u, v);
                double qd = 0.0; // omitted
                double qr = -4 * sigma * eps * delta_x * std::pow(u.val, 3) * grad_dot(u, v);
                double Qconv = xi * (T0 - u.val);
                double Qrz = sigma * eps / delta_z * (std::pow(T0, 4) - std::pow(u.val, 4));

                double val = (Rc + Qw + Qconv + Qrz) * v.val + qc + qd + qr;
                U(aa[0], aa[1]) += (u.val * v.val + steps.dt * inv * val) * w * J;

                double fval = - delta * 3e2 * r * fuel.val * v.val;
                F(aa[0], aa[1]) += (fuel.val * v.val + steps.dt * fval) * w * J;
                
                double wval = wind;
                W(aa[0], aa[1]) += (wind * v.val + steps.dt * wval) * w * J;

               
            }
        }
        executor.synchronized([&] { update_global_rhs(rhs, U, e); });
        executor.synchronized([&] { update_global_rhs(rhs_fuel, F, e); });
        executor.synchronized([&] { update_global_rhs(rhs_wind, W, e); });
    });
}

void after_step(int iter, double /*t*/) override {
    auto i = iter  +1;
    if (i % 10 == 0) {
        std::cout << "Step " << i << std::endl;
        output.to_file(u, "out_%d.data", i);
        output.to_file(fuel, "fuel_%d.data", i);
	output.to_file(wind, "wind_%d.data", i);
    }
}

    // double forcing(point_type x, double /*t*/) const {
    //     return 0;
    // }
};

}  // namespace ads::problems

#endif  // FIRE_FIRE_HPP
