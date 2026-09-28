#version 330 core
#define PI 3.14159265
#define coefficientsLength 11

out vec4 FragColor;

uniform vec2 iResolution;
uniform float iTime;

uniform float coefficients[coefficientsLength];

float dist(vec2 p1, vec2 p2) {
    return sqrt(pow(p1.x - p2.x, 2.0) + pow(p1.y - p2.y, 2.0));
}

float brightness(vec2 point) {
    float d = dist(vec2(0.0), point);
    return 0.8 * exp(-0.8 * d) + 0.2;
}

vec3 cColor(vec2 point) {
    // normalized angle of point in complex space to [0.0, 1.0]
    float angle = mod(((atan(point.y, point.x) + PI) / (2.0 * PI)), 1.0);

    // copied from online, gets HSV
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(vec3(angle) + K.xyz) * 6.0 - K.www);
    return clamp(p - K.xxx, 0.0, 1.0);
}

vec2 cPow(vec2 p, float power) {
    float r = pow(dist(vec2(0.0), p), power);
    float theta = power * atan(p.y, p.x);

    return r * vec2(cos(theta), sin(theta));
}

vec2 f(vec2 p) {
    // return cPow(p, 3.0) - vec2(sin(iTime * PI), 0.0);
    // return cPow(p, 3.0) - cPow(p, 2.0) - 5.0 * cPow(p, 1.0) - vec2(3.0, 0.0);
    // return cPow(p, 2.0) + cPow(p, 1.0) - vec2(3.0, 1.0);
    // return cPow(p, 2.0) + vec2(confA, 0.0);

    vec2 sum = vec2(0.0);
    for(int i=0; i < coefficientsLength; i++) {
        sum += coefficients[i] * cPow(p, float(coefficientsLength - 1 - i));
    }

    return sum;
}

void main()
{
    vec2 uv = gl_FragCoord.xy/iResolution.xy;
    vec2 pos;
    bool solution;
    if(uv.x >= 0.5) {
        solution = true;
        uv.x -= 0.5;
        uv.x *= 2;
        pos = 2.0 * ((uv) - vec2(0.5));
    } else {
        solution = false;
        uv.x *= 2;
        pos = 2.0 * (uv - vec2(0.5));
    }

    float scaleX = 5.0;
    float scaleY = 5.0;

    pos *= vec2(scaleX, scaleY);

    vec2 val = f(pos);
    vec3 color;
    if(solution) {
        color = cColor(val); // * brightness(val);
    } else {
        color = vec3(0.0);
    }

    if(abs(pos.x) <= 0.01 || abs(pos.y) <= 0.01) {
        color = vec3(1.0);
    }
    
    if(abs(mod(pos.x, 1.0)) <= 0.01 && abs(pos.y) <= 0.1) {
        color = vec3(1.0);
    }
    
    if(abs(mod(pos.y, 1.0)) <= 0.01 && abs(pos.x) <= 0.1) {
        color = vec3(1.0);
    }

    if (!solution) {
        float graphY = f(vec2(pos.x, 0.0)).x;
        float thickness = max(0.01, 0.5 * fwidth(graphY));

        if (abs(graphY - pos.y) <= thickness) {
            color = vec3(1.0);
        }
    }

    FragColor = vec4(color, 1.0);
}
