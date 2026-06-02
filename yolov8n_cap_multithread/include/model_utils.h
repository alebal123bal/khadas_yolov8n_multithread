#pragma once

#include <stdio.h>
#include <sys/time.h>
#include "rknn_api.h"

void           dump_tensor_attr(rknn_tensor_attr* attr);
double         __get_us(struct timeval t);
unsigned char* load_data(FILE* fp, size_t ofst, size_t sz);
unsigned char* load_model(const char* filename, int* model_size);
int            saveFloat(const char* file_name, float* output, int element_size);
