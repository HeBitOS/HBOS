#include <math.h>

double remquo(double x, double y, int *quo);

double remainder(double x, double y)
{
	int q;
	return remquo(x, y, &q);
}
