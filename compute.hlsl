const RWBuffer<float> input;
//RWBuffer<float> output;

void foo(int id) {
    float f = input[id];
    //output[id] = input[id];
}

[numthreads(1, 1, 1)]
void main(uint id : SV_DispatchThreadID){
}
