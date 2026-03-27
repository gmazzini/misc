#include <stdio.h>
#include <stdlib.h>
#include <string.h>


void loadcty(){
  FILE *f;
  char riga[100000],*dd[20],*p,*ff,cont[256],prefix[256],tmp[1024],*a,*b,*c,*base,*name;
  int i,dxcc,cqzone,ituzone,cq0,itu0;
  float latitude,longitude,gmtshift,lat0,lon0,gmt0;
  
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
    base=dd[0];
    name=dd[1];
    dxcc=atoi(dd[2]);
    cq0=atoi(dd[4]);
    itu0=atoi(dd[5]);
    lat0=(float)atof(dd[6]);
    lon0=(float)atof(dd[7]);
    gmt0=(float)atof(dd[8]);
    ff=strtok(dd[9]," ");
    while(ff){
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
      else cqzone=cq0;
      a=strchr(tmp,'[');
      if(a){
        b=strchr(a,']');
        *b=0;
        ituzone=atoi(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else ituzone=itu0;
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
        latitude=lat0;
        longitude=lon0;
      }
      a=strchr(tmp,'~');
      if(a){
        b=strchr(a+1,'~');
        *b=0;
        gmtshift=(float)atof(a+1);
        memmove(a,b+1,strlen(b+1)+1);
      }
      else gmtshift=gmt0;
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
