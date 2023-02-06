void foo() {
}

[numthreads(1, 1, 1)]
void compute_1(){
  foo();
}
