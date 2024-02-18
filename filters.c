#include <math.h>

#include "filters.h"

/* Allpass-based lowpass */
double lp_xh_primary = 0;
double lp_xh_secondary = 0;
double lp_c;

/* Needed for the monitor */
int left_old = 0;
int right_old = 0;

/* Discrete derivative */
int primary_old = 0;
int secondary_old = 0;

/* Exponential moving average */
int ema_primary_old = 0;
int ema_secondary_old = 0;

/* Allpass-based bandpass */
double bp_primary_xh[2] = { 0, 0 };
double bp_secondary_xh[2] = { 0, 0 };
double bp_c;

/* 
 * Applies a lowpass filter to the input signal x.
 * Wc is the normalized cut-off frequency 0 < Wc < 1, i.e. 2 * cutoff_freq / sampling_freq
 */
int aplowpass(const int x, double *lp_xh)
{
	int ap_y;
	int y;
	double lp_xh_new;

	lp_xh_new = x - lp_c * *lp_xh;
	ap_y = lp_c * lp_xh_new + *lp_xh;
	*lp_xh = lp_xh_new;

	y = 0.5 * (x + ap_y);

	return y;
}

/* 
 * Initializes the lowpass filter with a given sampling rate and cutoff-frequency 
 */
void aplowpass_init(const int cutoff_freq, const int sampling_freq)
{
	double Wc = 2.0 * cutoff_freq / sampling_freq;
	lp_c = (tan(M_PI * Wc / 2) - 1) / (tan(M_PI * Wc / 2) + 1);
}

/*
 * Computes an exponential moving average with the possibility to weight newly added
 * values with a factor alpha
 */
int ema(int x, int *ema_old, double alpha)
{
	int y = alpha * x + (1 - alpha) * *ema_old;
	*ema_old = y;
	return y;
}

/*
 * Computes an exponential moving average with the possibility to weight newly added
 * values with a factor alpha
 */
double emaf(double x, double *ema_old, double alpha)
{
	double y = alpha * x + (1 - alpha) * *ema_old;
	*ema_old = y;
	return y;
    }

/* 
 * Computes a simple derivative, i.e. the slope of the input signal  
 */
int discrete_derivative(int x, int *x_old)
{
	int y = x - *x_old;
	*x_old = x;
	return y;
}

/* 
 * Initializes the bandpass filter with a given sampling rate and bandwidth
 */
void apbandpass_init(const int bandwidth, const int sampling_freq)
{
	double Wb = 2.0 * bandwidth / sampling_freq;
	bp_c = (tan(M_PI * Wb / 2) - 1) / (tan(M_PI * Wb / 2) + 1);
}

/* 
 * Applies a bandpass filter to the input signal x.
 * Wc is the normalized center frequency 0 < Wc < 1, i.e. 2 * center_freq / sampling_freq
 */
int apbandpass(int x, double *xh, const int center_freq, const int sampling_freq)
{
	double Wc = 2.0 * center_freq / sampling_freq;
	double d = -cos(M_PI * Wc);
	double xh_new;
	int ap_y, y;

	xh_new = x - d * (1 - bp_c) * xh[0] + bp_c * xh[1];
	ap_y = -bp_c * xh_new + d * (1 - bp_c) * xh[0] + xh[1];

	xh[1] = xh[0];
	xh[0] = xh_new;

	y = 0.5 * (x - ap_y); // change to plus for bandreject

	return y;
}

/* 
 * Calculates a weighted highpass
 */
int highpass_weighted(int x, int *x_old, int *y_old, float alpha)
{
	int y;

	y = x - *x_old + alpha * *y_old;

	*y_old = y;
	*x_old = x;

	return y;
}
