#ifndef spring_h_INCLUDED
#define spring_h_INCLUDED

#include <cmath>

struct SpringParams {
    double response;        // Apple's "response" (seconds to settle)
    double dampingFraction; // Apple's dampingFraction (0 = very bouncy, 1 = critically damped)
};

struct SpringState {
    double value;     // position at time t
    double velocity;  // velocity at time t
};

static SpringState springEvaluate(
    double time,        // elapsed time (seconds)
    double x0,          // initial value
    double xTarget,     // final value
    double v0,          // initial velocity (units/sec)
    SpringParams params
) {
    double mass = 1.0; 
    double stiffness = (2 * M_PI / params.response) * (2 * M_PI / params.response) * mass;
    double damping = 2 * params.dampingFraction * std::sqrt(stiffness * mass);

    double deltaX = x0 - xTarget;

    double omega0 = std::sqrt(stiffness / mass);  
    double zeta   = damping / (2 * std::sqrt(stiffness * mass)); 

    SpringState result{};

    if (zeta < 1) {
        // Underdamped
        double omegaD = omega0 * std::sqrt(1 - zeta * zeta);
        double A = deltaX;
        double B = (v0 + zeta * omega0 * deltaX) / omegaD;

        double expTerm = std::exp(-zeta * omega0 * time);
        double cosTerm = std::cos(omegaD * time);
        double sinTerm = std::sin(omegaD * time);

        result.value = xTarget + expTerm * (A * cosTerm + B * sinTerm);

        // derivative (velocity)
        result.velocity =
            expTerm * ( -A * omegaD * sinTerm + B * omegaD * cosTerm )  // derivative of oscillatory part
            - zeta * omega0 * expTerm * (A * cosTerm + B * sinTerm);   // derivative of exponential part

    } else if (std::abs(zeta - 1.0) < 1e-6) {
        // Critically damped
        double A = deltaX;
        double B = v0 + omega0 * deltaX;

        double expTerm = std::exp(-omega0 * time);

        result.value = xTarget + expTerm * (A + B * time);
        result.velocity = expTerm * (B - omega0 * (A + B * time));

    } else {
        // Overdamped
        double omegaD = omega0 * std::sqrt(zeta * zeta - 1);
        double r1 = -omega0 * (zeta - std::sqrt(zeta * zeta - 1));
        double r2 = -omega0 * (zeta + std::sqrt(zeta * zeta - 1));

        double C1 = (deltaX * r2 - v0) / (r2 - r1);
        double C2 = (v0 - deltaX * r1) / (r2 - r1);

        result.value = xTarget + C1 * std::exp(r1 * time) + C2 * std::exp(r2 * time);
        result.velocity = C1 * r1 * std::exp(r1 * time) + C2 * r2 * std::exp(r2 * time);
    }

    return result;
}

// 10
//std::cout<<springEvaluate(0, 10, 100, 0, {100, 1}).value << std::endl;
// 21.818
//std::cout<<springEvaluate(10, 10, 100, 0, {100, 1}).value << std::endl;


#endif // spring_h_INCLUDED
