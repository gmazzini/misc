#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void loadcty(){
  FILE *f;
  char riga[100000],q[200000],*dd[20],*p,*ff,base[256],name[256],cont[256],prefix[256],tmp[1024],esc[512],*a,*b,*c;
  int i,dxcc,cqzone,ituzone;
  float latitude,longitude,gmtshift;
  
  f=fopen("cty.csv","r");
  while(fgets(riga,sizeof(riga),f)){
    for(i=0;i<20;i++)dd[i]=0;
    p=strtok(riga,",");
    i=0;
    while(p&&i<20){
      dd[i]=p;
      i++;
      p=strtok(NULL,",");
    }
    if(!dd[9])continue;
    dd[9][strcspn(dd[9],"\r\n")]=0;
    if(strlen(dd[9])>0)dd[9][strlen(dd[9])-1]=0;
    ff=strtok(dd[9]," ");
    while(ff){
      strcpy(base,dd[0]);
      strcpy(name,dd[1]);
      dxcc=atoi(dd[2]);
      strcpy(tmp,ff);
      a=strchr(tmp,'{');
      if(a){
        b=strchr(a,'}');
        strncpy(cont,a+1,b-a-1);
        cont[b-a-1]=0;
        memmove(a,b+1,strlen(b+1)+1);
      }
      else strcpy(cont,dd[3]);
      a=strchr(tmp,'(');
      if(a){
        b=strchr(a,')');
        *b=0;
        cqzone=atoi(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else cqzone=atoi(dd[4]);
      a=strchr(tmp,'[');
      if(a){
        b=strchr(a,']');
        *b=0;
        ituzone=atoi(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else ituzone=atoi(dd[5]);
      a=strchr(tmp,'<');
      if(a){
        b=strchr(a,'>');
        c=strchr(a,'/');
        *c=0;
        *b=0;
        latitude=(float)atof(a+1);
        longitude=(float)atof(c+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else{
        latitude=(float)atof(dd[6]);
        longitude=(float)atof(dd[7]);
      }
      a=strchr(tmp,'~');
      if(a){
        b=strchr(a+1,'~');
        *b=0;
        gmtshift=(float)atof(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else gmtshift=(float)atof(dd[8]);
      if(tmp[0]=='=')strcpy(prefix,tmp+1);
      else strcpy(prefix,tmp);
      printf("%s %d %s %d %d\n",prefix,dxcc,cont,cqzone,ituzone);
      ff=strtok(NULL," ");
    }
  }
  fclose(f);
}

int main(){
    loadcty();
}
