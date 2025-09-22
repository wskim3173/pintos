#define F (1 << 14)
#define INT_TO_FP(n) ((n) * F) // Convert n to fixed point
#define FP_TO_INT_ZERO(x) ((x) / F) // Convert x to integer (rounding toward zero)
#define FP_TO_INT_NEAR(x) ((x) >= 0 ? (((x) + F/2) / F) : (((x) - F/2) / F)) // Convert x to integer (rounding to nearest)
#define ADD_FP(x,y) ((x) + (y)) // Add x and y
#define SUB_FP(x,y) ((x) - (y)) // Subtract y from x
#define ADD_MIX(x,n) ((x) + (n) * F) // Add x and n
#define SUB_MIX(x,n) ((x) - (n) * F) // Subtract n from x
#define MUL_FP(x,y) ((int64_t)(x)) * (y) / F // Multiply x by y
#define MUL_MIX(x,n) ((x) * (n)) // Multiply x by n
#define DIV_FP(x,y) ((int64_t)(x)) * F / (y) // Divide x by y
#define DIV_MIX(x,n) ((x) / (n)) // Divide x by n