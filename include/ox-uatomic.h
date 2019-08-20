#ifndef _UATOMIC_H
#define _UATOMIC_H

/**
 * Atomic type.
 */

typedef struct {
    volatile int counter;
} u_atomic_t;

#define U_ATOMIC_INIT(i)  { (i) }
#define U_ATOMIC_INIT_RUNTIME(i)  (i)

/**
 * Read atomic variable
 * @param v pointer of type atomic_t
 *
 * Atomically reads the value of @v.
 */
#define u_atomic_read(v) ((v)->counter)

/**
 * Set atomic variable
 * @param v pointer of type atomic_t
 * @param i required value
 */
#define u_atomic_set(v,i) (((v)->counter) = (i))

/**
 * Add to the atomic variable
 * @param i integer value to add
 * @param v pointer of type atomic_t
 */
static inline void u_atomic_add( int i, u_atomic_t *v )
{
         (void)__sync_add_and_fetch(&v->counter, i);
}

/**
 * Subtract the atomic variable
 * @param i integer value to subtract
 * @param v pointer of type atomic_t
 *
 * Atomically subtracts @i from @v.
 */
static inline void u_atomic_sub( int i, u_atomic_t *v )
{
        (void)__sync_sub_and_fetch(&v->counter, i);
}

/**
 * Subtract value from variable and test result
 * @param i integer value to subtract
 * @param v pointer of type atomic_t
 *
 * Atomically subtracts @i from @v and returns
 * true if the result is zero, or false for all
 * other cases.
 */
static inline int u_atomic_sub_and_test( int i, u_atomic_t *v )
{
        return !(__sync_sub_and_fetch(&v->counter, i));
}

/**
 * Increment atomic variable
 * @param v pointer of type atomic_t
 *
 * Atomically increments @v by 1.
 */
static inline void u_atomic_inc( u_atomic_t *v )
{
       (void)__sync_fetch_and_add(&v->counter, 1);
}

/**
 * @brief decrement atomic variable
 * @param v: pointer of type atomic_t
 *
 * Atomically decrements @v by 1.  Note that the guaranteed
 * useful range of an atomic_t is only 24 bits.
 */
static inline void u_atomic_dec( u_atomic_t *v )
{
       (void)__sync_fetch_and_sub(&v->counter, 1);
}

/**
 * @brief Decrement and test
 * @param v pointer of type atomic_t
 *
 * Atomically decrements @v by 1 and
 * returns true if the result is 0, or false for all other
 * cases.
 */
static inline int u_atomic_dec_and_test( u_atomic_t *v )
{
       return !(__sync_sub_and_fetch(&v->counter, 1));
}

/**
 * @brief Increment and test
 * @param v pointer of type atomic_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
static inline int u_atomic_inc_and_test( u_atomic_t *v )
{
      return !(__sync_add_and_fetch(&v->counter, 1));
}

/**
 * @brief add and test if negative
 * @param v pointer of type atomic_t
 * @param i integer value to add
 *
 * Atomically adds @i to @v and returns true
 * if the result is negative, or false when
 * result is greater than or equal to zero.
 */
static inline int u_atomic_add_negative( int i, u_atomic_t *v )
{
       return (__sync_add_and_fetch(&v->counter, i) < 0);
}

#endif /* UATOMIC_H */
