#ifndef ZSHARP_STYLE_TRANSITION_H
#define ZSHARP_STYLE_TRANSITION_H
typedef struct ZSharpStyleTween {
    double from[6], target[6], started;
    int initialized;
} ZSharpStyleTween;
int zsharp_style_transition_parse(const char *text, double *seconds, int *easing);
double zsharp_style_ease(double t, int easing);
void zsharp_style_tween(ZSharpStyleTween *tween, const double target[6], double now,
                        double duration, int easing, double output[6]);
#endif
