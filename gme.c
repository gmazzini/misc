#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <zip.h>

#define TOKEN_FILE "/home/www/data/google_access_token"
#define SHEET_ID "1RF4N-T2NR2UHai70AzTzwuLXowkLlOQWvFyb8AaE1xg"
#define SHEET_NAME "prova"
#define OFFSET 0

struct mem{
  char *ptr;
  size_t len;
};

static void mem_init(struct mem *m){
  m->len=0;
  m->ptr=(char*)malloc(1);
  if(m->ptr)m->ptr[0]='\0';
}

static size_t write_cb(void *contents,size_t size,size_t nmemb,void *userp){
  size_t realsize;
  struct mem *m;
  char *p;

  realsize=size*nmemb;
  m=(struct mem*)userp;
  p=(char*)realloc(m->ptr,m->len+realsize+1);
  if(!p)return 0;
  m->ptr=p;
  memcpy(m->ptr+m->len,contents,realsize);
  m->len=m->len+realsize;
  m->ptr[m->len]='\0';
  return realsize;
}

static int read_access_token(char *buf,size_t buflen){
  FILE *fp;

  fp=fopen(TOKEN_FILE,"r");
  if(!fp)return 0;
  if(!fgets(buf,(int)buflen,fp)){
    fclose(fp);
    return 0;
  }
  fclose(fp);
  buf[strcspn(buf,"\r\n")]='\0';
  return buf[0]!='\0';
}

int main(int argc,char *argv[]){
  CURL *curl;
  struct curl_slist *headers;
  struct mem body_auth;
  struct mem body_req;
  struct mem body_gs;
  char auth_post[1024];
  char req_post[2048];
  char gs_post[32768];
  char auth_header[8192];
  char gs_auth_header[2048];
  char gs_url[2048];
  char gs_range[128];
  char google_access_token[512];
  char token[4096];
  char data_fmt[16];
  char *p;
  char *q;
  char *r;
  char *s;
  char *e;
  char *b64;
  unsigned char *zipbuf;
  unsigned char *filebuf;
  zip_error_t ze;
  zip_source_t *zs;
  zip_t *za;
  zip_file_t *zf;
  zip_stat_t st;
  zip_int64_t nr;
  long http;
  int len;
  int outlen;
  int val;
  int valb;
  int c;
  int i;
  int j;
  int count;
  int err;
  int giorno;
  int riga;
  char prices[96][64];
  char oneprice[64];
  char *json_buf;

  if(argc!=4){
    fprintf(stderr,"Usage: %s LOGIN PASSWORD YYYYMMDD\n",argv[0]);
    return 1;
  }

  if(strlen(argv[3])!=8){
    fprintf(stderr,"date format must be YYYYMMDD\n");
    return 1;
  }

  giorno=(argv[3][6]-'0')*10+(argv[3][7]-'0');
  riga=90-giorno+OFFSET;

  if(riga<1){
    fprintf(stderr,"invalid row %d\n",riga);
    return 1;
  }

  data_fmt[0]=argv[3][6];
  data_fmt[1]=argv[3][7];
  data_fmt[2]='/';
  data_fmt[3]=argv[3][4];
  data_fmt[4]=argv[3][5];
  data_fmt[5]='/';
  data_fmt[6]=argv[3][0];
  data_fmt[7]=argv[3][1];
  data_fmt[8]=argv[3][2];
  data_fmt[9]=argv[3][3];
  data_fmt[10]=0;

  snprintf(gs_range,sizeof(gs_range),"%s!A%d:CS%d",SHEET_NAME,riga,riga);

  if(!read_access_token(google_access_token,sizeof(google_access_token))){
    fprintf(stderr,"google access token not found\n");
    return 1;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);

  mem_init(&body_auth);
  mem_init(&body_req);
  mem_init(&body_gs);

  headers=NULL;

  snprintf(auth_post,sizeof(auth_post),"{\"Login\":\"%s\",\"Password\":\"%s\"}",argv[1],argv[2]);

  curl=curl_easy_init();
  if(!curl)return 1;

  headers=curl_slist_append(headers,"Content-Type: application/json");

  curl_easy_setopt(curl,CURLOPT_URL,"https://api.mercatoelettrico.org/request/api/v1/Auth");
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,auth_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(auth_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body_auth);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK){
    fprintf(stderr,"auth curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  p=strstr(body_auth.ptr,"\"token\"");
  if(p==NULL){
    printf("%s\n",body_auth.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  p=strchr(p,':');
  p=strchr(p,'"');
  p=p+1;
  q=strchr(p,'"');

  i=0;
  while(p<q&&i<(int)sizeof(token)-1){
    token[i]=*p;
    i++;
    p++;
  }
  token[i]=0;

  printf("TOKEN=%s\n",token);

  snprintf(req_post,sizeof(req_post),
    "{\"Platform\":\"PublicMarketResults\","
    "\"Segment\":\"MGP\","
    "\"DataName\":\"ME_ZonalPrices\","
    "\"IntervalStart\":\"%s\","
    "\"IntervalEnd\":\"%s\","
    "\"Attributes\":{\"GranularityType\":\"PT15\"}}",
    argv[3],argv[3]);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  curl=curl_easy_init();
  if(!curl){
    curl_global_cleanup();
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  snprintf(auth_header,sizeof(auth_header),"Authorization: Bearer %s",token);
  headers=curl_slist_append(headers,auth_header);

  curl_easy_setopt(curl,CURLOPT_URL,"https://api.mercatoelettrico.org/request/api/v1/RequestData");
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,req_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(req_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body_req);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK){
    fprintf(stderr,"request curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  p=strstr(body_req.ptr,"\"contentResponse\"");
  if(p==NULL){
    printf("%s\n",body_req.ptr);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  p=strchr(p,':');
  p=strchr(p,'"');
  p=p+1;
  q=strchr(p,'"');

  len=(int)(q-p);
  b64=(char*)malloc(len+1);
  for(i=0;i<len;i++)b64[i]=p[i];
  b64[len]=0;

  zipbuf=(unsigned char*)malloc((len*3)/4+4);
  val=0;
  valb=-8;
  outlen=0;

  for(i=0;i<len;i++){
    c=b64[i];
    if(c>='A'&&c<='Z')c=c-'A';
    else if(c>='a'&&c<='z')c=c-'a'+26;
    else if(c>='0'&&c<='9')c=c-'0'+52;
    else if(c=='+')c=62;
    else if(c=='/')c=63;
    else if(c=='=')break;
    else continue;

    val=(val<<6)+c;
    valb=valb+6;

    if(valb>=0){
      zipbuf[outlen]=(unsigned char)((val>>valb)&255);
      outlen++;
      valb=valb-8;
    }
  }

  zip_error_init(&ze);
  zs=zip_source_buffer_create(zipbuf,outlen,0,&ze);
  if(zs==NULL){
    fprintf(stderr,"zip source error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  za=zip_open_from_source(zs,0,&ze);
  if(za==NULL){
    fprintf(stderr,"zip open error\n");
    zip_source_free(zs);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  nr=zip_get_num_entries(za,0);
  if(nr<1){
    fprintf(stderr,"zip empty\n");
    zip_close(za);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  zip_stat_init(&st);
  err=zip_stat_index(za,0,0,&st);
  if(err!=0){
    fprintf(stderr,"zip stat error\n");
    zip_close(za);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  zf=zip_fopen_index(za,0,0);
  if(zf==NULL){
    fprintf(stderr,"zip fopen error\n");
    zip_close(za);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 1;
  }

  filebuf=(unsigned char*)malloc((size_t)st.size+1);
  zip_fread(zf,filebuf,st.size);
  filebuf[st.size]=0;
  zip_fclose(zf);
  zip_close(za);

  json_buf=(char*)filebuf;
  r=json_buf;
  count=0;

  while(1){
    r=strstr(r,"\"Zone\":\"PUN\"");
    if(r==NULL)break;

    s=strstr(r,"\"Price\"");
    if(s==NULL)break;
    s=strchr(s,':');
    s=s+1;
    while(*s==' ')s=s+1;
    e=s;
    while(*e!=','&&*e!='}'&&*e!=0)e=e+1;

    j=0;
    while(s<e&&j<(int)sizeof(oneprice)-1){
      oneprice[j]=*s;
      j++;
      s=s+1;
    }
    oneprice[j]=0;

    if(count<96){
      strcpy(prices[count],oneprice);
      count++;
    }

    r=r+10;
  }

  if(count!=96){
    fprintf(stderr,"found %d prices, expected 96\n",count);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(b64);
    free(zipbuf);
    free(filebuf);
    return 1;
  }

  gs_post[0]=0;
  strcat(gs_post,"{\"valueInputOption\":\"RAW\",\"data\":[{\"range\":\"");
  strcat(gs_post,gs_range);
  strcat(gs_post,"\",\"majorDimension\":\"ROWS\",\"values\":[[\"");
  strcat(gs_post,data_fmt);
  strcat(gs_post,"\"");

  for(i=0;i<96;i++){
    strcat(gs_post,",");
    strcat(gs_post,prices[i]);
  }

  strcat(gs_post,"]]}]}");

  snprintf(gs_url,sizeof(gs_url),"https://sheets.googleapis.com/v4/spreadsheets/%s/values:batchUpdate",SHEET_ID);
  snprintf(gs_auth_header,sizeof(gs_auth_header),"Authorization: Bearer %s",google_access_token);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  curl=curl_easy_init();
  if(!curl){
    curl_global_cleanup();
    free(b64);
    free(zipbuf);
    free(filebuf);
    return 1;
  }

  headers=NULL;
  headers=curl_slist_append(headers,"Content-Type: application/json");
  headers=curl_slist_append(headers,gs_auth_header);

  curl_easy_setopt(curl,CURLOPT_URL,gs_url);
  curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
  curl_easy_setopt(curl,CURLOPT_POST,1L);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDS,gs_post);
  curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(gs_post));
  curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_cb);
  curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body_gs);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
  curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
  curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,15L);
  curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);

  if(curl_easy_perform(curl)!=CURLE_OK){
    fprintf(stderr,"google sheets curl error\n");
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    free(b64);
    free(zipbuf);
    free(filebuf);
    return 1;
  }

  curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http);
  printf("%s\n",body_gs.ptr?body_gs.ptr:"");
  printf("COUNT=%d\n",count);
  printf("ROW=%d\n",riga);
  printf("HTTP=%ld\n",http);

  free(body_auth.ptr);
  free(body_req.ptr);
  free(body_gs.ptr);
  free(b64);
  free(zipbuf);
  free(filebuf);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();
  return 0;
}
