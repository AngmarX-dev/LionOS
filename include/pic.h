#ifndef LIONOS_PIC_H
#define LIONOS_PIC_H

void pic_init(void);
/* Returns 0 on verified masking, -1 if the previous masks were restored,
 * and -2 if restoration itself could not be verified. */
int pic_mask_all(void);

#endif
