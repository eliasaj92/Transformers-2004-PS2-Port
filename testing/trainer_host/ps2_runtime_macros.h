#pragma once
#define GPR_U32(context,reg) ((context)->r[reg])
#define SET_GPR_S32(context,reg,value) ((context)->r[reg]=uint32_t(value))
