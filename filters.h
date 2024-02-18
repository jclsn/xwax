#ifndef FILTER_H

#define FILTER_H

#include <stdbool.h>

/* Allpass-based lowpass */
extern double lp_xh_primary; 
extern double lp_xh_secondary;
extern double lp_c;

/* Needed for the monitor */
extern int left_old;
extern int right_old; 

/* Discrete derivative */
extern int primary_old;
extern int secondary_old;

/* Exponential moving average */
extern int ema_primary_old;
extern int ema_secondary_old;

/* Allpass-based bandpass */
extern double bp_primary_xh[2];
extern double bp_secondary_xh[2];
extern double bp_c;

/* Simple weighted highpass */
extern int x_old_primary;
extern int y_old_primary;
extern int x_old_secondary;
extern int y_old_secondary;

int aplowpass(const int x, double *lp_xh);
void aplowpass_init(const int cutoff_freq, const int sampling_freq);
int ema(int x, int *ema_old, double alpha);
double emaf(double x, double *ema_old, double alpha);
int discrete_derivative(int x, int *x_old);
void apbandpass_init(const int bandwidth, const int sampling_freq);
int apbandpass(int x, double *xh, const int center_freq, const int sampling_freq);
int highpass_weighted(int x, int *x_old, int *y_old, float alpha);

#endif /* end of include guard FILTER_H */
