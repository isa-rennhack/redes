#include <fcntl.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

int main (int argc, char *argv[])

{

  int cont, i, src, dest, j, k ;
    int buffer [2048] ;
    
    printf (" Comecei a copia\n");    
    sleep (10) ;
    if (argc != 3) 
       {
         printf (" Numero de argumentos insuficientes\n") ;
         exit (1) ;
       } 
    src = open (argv[1],O_RDONLY);
    if (src == -1)
       {
         printf("Impossivel abrir o arquivo %s\n", argv[1]);
         exit (1); 
       }
    dest = creat(argv[2], 0666);
    if (dest == -1)
       {
         printf(" Impossivel criar o arquivo %s\n", argv[2]); 
         exit (1) ;
       }
    
    while ((cont = read(src, &buffer, sizeof(buffer))) > 0 ){
            send;
            write (dest, &buffer, cont) ;
            }
    printf(" Terminei a copia\n");
return 0;
}
 