<?php
include("local.php");

$con=mysqli_connect($dbhost,$dbuser,$dbpassword,$dbname);
mysqli_set_charset($con,'utf8mb4');
if(!$con) die("Errore DB");

$timeout=600; // 10 minuti
$cell="";
$tmpkey="";
$authed=0;
$editmode=0;
$ed_origin="";
$ed_destination="";
$ed_description="";
$msg="";

$sort="origin";
$dir="asc";

function genorigin($len=8){
  $chars='0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
  $out='';
  for($i=0;$i<$len;$i++) $out.=$chars[random_int(0,strlen($chars)-1)];
  return $out;
}

function nextdir($field,$sort,$dir){
  if($field==$sort) return ($dir=="asc") ? "desc" : "asc";
  return "asc";
}

function arrow($field,$sort,$dir){
  if($field!=$sort) return "";
  return ($dir=="asc") ? " ↑" : " ↓";
}

function h($x){
  return htmlspecialchars($x ?? "", ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
}

/* Origin cliccabile come https://origin */
$scheme = "https";

/* ordinamento */
if(!empty($_POST['sort']) && in_array($_POST['sort'],["origin","destination","description","hit"])) {
  $sort=$_POST['sort'];
}
if(!empty($_POST['dir']) && ($_POST['dir']=="asc" || $_POST['dir']=="desc")) {
  $dir=$_POST['dir'];
}

/* token già presente */
if(!empty($_POST['tmpkey'])){
  $tmpkey=preg_replace('/[^a-fA-F0-9]/','',$_POST['tmpkey']);
  if(strlen($tmpkey)==32){
    $tmpkeyesc=mysqli_real_escape_string($con,$tmpkey);
    $q=mysqli_query($con,"SELECT cell,tmpepoch FROM auth_redirect WHERE tmpkey='$tmpkeyesc' LIMIT 1");
    $r=mysqli_fetch_assoc($q);
    mysqli_free_result($q);

    if($r!=null && time()-(int)$r["tmpepoch"]<$timeout){
      $authed=1;
      $cell=$r["cell"];
      mysqli_query($con,"UPDATE auth_redirect SET tmpepoch=".time()." WHERE tmpkey='$tmpkeyesc' LIMIT 1");
    } else {
      $msg="<span class='msg error'>Sessione scaduta, rifare autenticazione</span>";
    }
  } else {
    $msg="<span class='msg error'>Sessione non valida, rifare autenticazione</span>";
  }
}

/* autenticazione OTP se non già autenticato */
if($authed==0 && !empty($_POST['cell'])){
  $cell=preg_replace('/\D+/','',$_POST['cell']);
  if(substr($cell,0,2)=="39" && strlen($cell)==12) $cell=substr($cell,2);

  if(!preg_match('/^\d{9,10}$/',$cell)){
    $msg="<span class='msg error'>Numero non valido</span>";
  } else {
    $cellesc=mysqli_real_escape_string($con,$cell);
    $q=mysqli_query($con,"SELECT cell FROM auth_redirect WHERE cell='$cellesc' LIMIT 1");
    $r=mysqli_fetch_assoc($q);
    mysqli_free_result($q);

    if($r==null){
      $msg="<span class='msg error'>Numero non autorizzato</span>";
    } else {
      $otp=sprintf("%05d",rand(0,99999));
      $epoch=time();
      $look=0;

      echo "<!doctype html><html lang='it'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>OTP</title>
      <style>
      body{font-family:Arial,sans-serif;margin:30px;background:#f5f7fb;color:#1f2937}
      .otpbox{max-width:700px;margin:auto;background:#fff;border:1px solid #dbe3ef;border-radius:14px;padding:24px;box-shadow:0 8px 24px rgba(0,0,0,.06)}
      .note{margin-top:14px;color:#6b7280;font-size:14px}
      .count{margin-top:14px;font-size:18px;font-weight:bold;color:#2563eb}
      a{color:#2563eb;text-decoration:none}
      a:hover{text-decoration:underline}
      </style>
      <script>
      let sec=90;
      function tick(){
        let e=document.getElementById('countdown');
        if(!e) return;
        e.textContent=sec;
        if(sec<=0){
          window.location='prova.php';
          return;
        }
        sec--;
        setTimeout(tick,1000);
      }
      </script>
      </head><body onload='tick()'>";

      echo "<div class='otpbox'><div id='otpbox'>Utente <b>".h($cell)."</b><br><br>Invia via SMS il codice <b>".h($otp)."</b> al numero di autenticazione <b>3294296486</b> entro <span id='countdown'>90</span> secondi</div><div class='note'>Usando questo servizio dichiari di aver preso visione dell’<a href='https://www.chaos.cc/privacy.html' target='_blank'>informativa privacy</a>.</div></div>";
      @ob_flush(); @flush();

      for($i=0;$i<90 && $look==0;$i++){
        $fp=@fopen("/home/www/data/sms/39".$cell,"r");
        if($fp!==false){
          $line=fgets($fp);
          fclose($fp);
          $vv=explode(",",$line);
          if(count($vv)>=2 && trim($vv[0])==$otp && $epoch-(int)$vv[1]<90){
            $look=1;
            break;
          }
        }
        sleep(1);
      }

      if($look==1){
        echo "<script>document.getElementById('otpbox').innerHTML='Autenticazione riuscita';</script>";
        $tmpkey=bin2hex(random_bytes(16)); // 32 char hex
        $tmpkeyesc=mysqli_real_escape_string($con,$tmpkey);
        mysqli_query($con,"UPDATE auth_redirect SET tmpkey='$tmpkeyesc', tmpepoch=".time()." WHERE cell='$cellesc' LIMIT 1");
        $authed=1;
        $msg="<span class='msg success'>Autenticazione riuscita</span>";
      } else {
        echo "<script>document.getElementById('otpbox').innerHTML='<span style=\"color:#b91c1c;font-weight:bold;\">OTP scaduto</span>';</script>";
        echo "<script>setTimeout(function(){ window.location='prova.php'; }, 1200);</script>";
        echo "</body></html>";
        mysqli_close($con);
        exit;
      }
      echo "</body></html>";
    }
  }
}

/* azioni */
if($authed){
  $cellesc=mysqli_real_escape_string($con,$cell);

  if(!empty($_POST['mode']) && $_POST['mode']=="insert"){
    $destination=trim($_POST['destination']);
    $description=trim($_POST['description']);

    if($destination==""){
      $msg="<span class='msg error'>Destination obbligatoria</span>";
    } else {
      $destinationesc=mysqli_real_escape_string($con,$destination);
      $descriptionesc=mysqli_real_escape_string($con,$description);

      $ok=0;
      for($k=0;$k<20 && $ok==0;$k++){
        $origin=genorigin(8);
        $originesc=mysqli_real_escape_string($con,$origin);
        mysqli_query($con,"INSERT INTO redirect(origin,destination,hit,description,cell)
          VALUES('$originesc','$destinationesc',0,'$descriptionesc','$cellesc')");
        if(mysqli_error($con)==""){
          $ok=1;
          $msg="<span class='msg success'>Inserito: <b>".h($origin)."</b></span>";
        }
      }
      if($ok==0) $msg="<span class='msg error'>Errore inserimento</span>";
    }
  }

  if(!empty($_POST['mode']) && $_POST['mode']=="edit"){
    $origin=trim($_POST['origin']);
    $originesc=mysqli_real_escape_string($con,$origin);

    $q=mysqli_query($con,"SELECT * FROM redirect WHERE origin='$originesc' AND cell='$cellesc' LIMIT 1");
    $r=mysqli_fetch_assoc($q);
    mysqli_free_result($q);

    if($r!=null){
      $editmode=1;
      $ed_origin=$r["origin"];
      $ed_destination=$r["destination"];
      $ed_description=$r["description"];
    }
  }

  if(!empty($_POST['mode']) && $_POST['mode']=="update"){
    $oldorigin=trim($_POST['oldorigin']);
    $destination=trim($_POST['destination']);
    $description=trim($_POST['description']);

    if($destination==""){
      $msg="<span class='msg error'>Destination obbligatoria</span>";
      $editmode=1;
      $ed_origin=$oldorigin;
      $ed_destination=$destination;
      $ed_description=$description;
    } else {
      $oldoriginesc=mysqli_real_escape_string($con,$oldorigin);
      $destinationesc=mysqli_real_escape_string($con,$destination);
      $descriptionesc=mysqli_real_escape_string($con,$description);

      mysqli_query($con,"UPDATE redirect
        SET destination='$destinationesc', description='$descriptionesc'
        WHERE origin='$oldoriginesc' AND cell='$cellesc' LIMIT 1");

      if(mysqli_error($con)=="") $msg="<span class='msg success'>Modificato</span>";
      else $msg="<span class='msg error'>Errore modifica: ".h(mysqli_error($con))."</span>";
    }
  }

  if(!empty($_POST['mode']) && $_POST['mode']=="delete"){
    $origin=trim($_POST['origin']);
    $originesc=mysqli_real_escape_string($con,$origin);

    mysqli_query($con,"DELETE FROM redirect WHERE origin='$originesc' AND cell='$cellesc' LIMIT 1");
    if(mysqli_error($con)=="") $msg="<span class='msg success'>Cancellato</span>";
    else $msg="<span class='msg error'>Errore cancellazione: ".h(mysqli_error($con))."</span>";
  }
}

/* ordinamento SQL */
$ordercol="origin";
if($sort=="origin") $ordercol="origin";
if($sort=="destination") $ordercol="destination";
if($sort=="description") $ordercol="description";
if($sort=="hit") $ordercol="hit";

$orderdir=($dir=="desc") ? "DESC" : "ASC";

/* output */
echo '<!doctype html>
<html lang="it">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gestione redirect</title>
<style>
:root{
  --bg:#f5f7fb;
  --card:#ffffff;
  --line:#dbe3ef;
  --text:#1f2937;
  --muted:#6b7280;
  --link:#2563eb;
  --okbg:#eaf8ef;
  --ok:#166534;
  --errbg:#fdecec;
  --err:#b91c1c;
  --shadow:0 10px 30px rgba(0,0,0,.06);
}
body{
  font-family:Arial,sans-serif;
  line-height:1.5;
  margin:0;
  background:var(--bg);
  color:var(--text);
}
*{ box-sizing:border-box; }
.wrap{
  width:100%;
  max-width:1180px;
  margin:0 auto;
  padding:24px;
}
.card{
  background:var(--card);
  border:1px solid var(--line);
  border-radius:16px;
  padding:20px;
  box-shadow:var(--shadow);
  margin-bottom:18px;
}
h1,h2{
  margin-top:0;
}
.meta{
  color:var(--muted);
  margin-bottom:6px;
}
input[type=text],input[type=url]{
  width:100%;
  padding:10px 12px;
  margin-bottom:12px;
  border:1px solid #cbd5e1;
  border-radius:10px;
  background:#fff;
}
input[readonly]{
  background:#f8fafc;
}
input[type=submit]{
  padding:10px 16px;
  margin-right:6px;
  border:1px solid #cbd5e1;
  border-radius:10px;
  background:#fff;
  cursor:pointer;
}
input[type=submit]:hover{
  background:#f8fafc;
}
table{
  border-collapse:collapse;
  width:100%;
  table-layout:fixed;
  margin-top:8px;
}
th,td{
  border-bottom:1px solid var(--line);
  padding:10px 8px;
  text-align:left;
  vertical-align:top;
  overflow-wrap:anywhere;
  word-break:break-word;
}
th{
  background:#f8fafc;
}
form{
  margin:0;
}
.sortbtn{
  background:none;
  border:none;
  padding:0;
  font:inherit;
  color:var(--link);
  cursor:pointer;
}
.actions form{
  display:inline;
}
a{
  color:var(--link);
  text-decoration:none;
}
a:hover{
  text-decoration:underline;
}
.msg{
  display:inline-block;
  padding:10px 12px;
  border-radius:10px;
  margin-bottom:10px;
}
.success{
  background:var(--okbg);
  color:var(--ok);
}
.error{
  background:var(--errbg);
  color:var(--err);
}
.grid{
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:18px;
}
@media (max-width: 900px){
  .grid{
    grid-template-columns:1fr;
  }
}
</style>
</head>
<body>
<div class="wrap">';

echo "<div class='card'>";
echo "<h1>Gestione redirect</h1>";
if($msg!="") echo "<div>$msg</div>";
echo "</div>";

if(!$authed){
  echo "<div class='card'>";
  echo "<div class='meta'>Usando questo servizio dichiari di aver preso visione dell’<a href='https://www.chaos.cc/privacy.html' target='_blank'>informativa privacy</a>.</div><br>";
  echo '<form method="post">
  <label>Cellulare</label><br>
  <input type="text" name="cell" required>
  <input type="submit" value="Accedi">
  </form>';
  echo "</div>";
  echo '</div></body></html>';
  mysqli_close($con);
  exit;
}

echo "<div class='card'>";
echo "<div class='meta'>Cellulare autenticato</div>";
echo "<div><b>".h($cell)."</b></div>";
echo "</div>";

echo "<div class='card'>";
echo "Informativa privacy: <a href='https://www.chaos.cc/privacy.html' target='_blank'>https://www.chaos.cc/privacy.html</a>";
echo "</div>";

echo "<div class='grid'>";

echo "<div class='card'>";
echo '<form method="post">';
echo '<input type="hidden" name="tmpkey" value="'.h($tmpkey).'">';
echo '<input type="hidden" name="sort" value="'.h($sort).'">';
echo '<input type="hidden" name="dir" value="'.h($dir).'">';

if($editmode){
  echo '<input type="hidden" name="mode" value="update">';
  echo '<input type="hidden" name="oldorigin" value="'.h($ed_origin).'">';
  echo '<h2>Modifica redirect</h2>';
  echo 'Origin<br><input type="text" value="'.h($ed_origin).'" readonly><br>';
  echo 'Destination<br><input type="url" name="destination" value="'.h($ed_destination).'" required><br>';
  echo 'Description<br><input type="text" name="description" value="'.h($ed_description).'"><br>';
  echo '<input type="submit" value="Salva modifica">';
} else {
  echo '<input type="hidden" name="mode" value="insert">';
  echo '<h2>Nuovo redirect</h2>';
  echo 'Origin<br><input type="text" value="automatico, 8 caratteri" readonly><br>';
  echo 'Destination<br><input type="url" name="destination" required><br>';
  echo 'Description<br><input type="text" name="description"><br>';
  echo '<input type="submit" value="Inserisci">';
}
echo '</form>';
echo "</div>";

echo "<div class='card'>";
echo "<h2>Legenda</h2>";
echo "<div class='meta'>Origin è il link breve generato automaticamente.</div>";
echo "<div class='meta'>Destination è l’URL reale di destinazione.</div>";
echo "<div class='meta'>Description è una nota libera per ricordarti a cosa serve il link.</div>";
echo "</div>";

echo "</div>";

echo "<div class='card'>";

$cellesc=mysqli_real_escape_string($con,$cell);
$q=mysqli_query($con,"SELECT origin,destination,hit,description FROM redirect WHERE cell='$cellesc' ORDER BY $ordercol $orderdir, origin ASC");

echo "<h2>I tuoi redirect</h2>";
echo "<table><tr>";

foreach (["origin"=>"Origin","destination"=>"Destination","hit"=>"Hit","description"=>"Description"] as $field=>$label) {
  echo "<th>
  <form method='post'>
    <input type='hidden' name='tmpkey' value='".h($tmpkey)."'>
    <input type='hidden' name='sort' value='".h($field)."'>
    <input type='hidden' name='dir' value='".h(nextdir($field,$sort,$dir))."'>
    <button class='sortbtn' type='submit'>".$label.arrow($field,$sort,$dir)."</button>
  </form>
  </th>";
}
echo "<th>Azione</th></tr>";

for(;;){
  $r=mysqli_fetch_assoc($q);
  if($r==null) break;

  $shortUrl = $scheme . "://" . $r["origin"];

  echo "<tr>";
  echo "<td><a href='".h($shortUrl)."' target='_blank'>".h($r["origin"])."</a></td>";
  echo "<td><a href='".h($r["destination"])."' target='_blank'>".h($r["destination"])."</a></td>";
  echo "<td>".(int)$r["hit"]."</td>";
  echo "<td>".h($r["description"])."</td>";
  echo "<td class='actions'>
    <form method='post'>
      <input type='hidden' name='tmpkey' value='".h($tmpkey)."'>
      <input type='hidden' name='sort' value='".h($sort)."'>
      <input type='hidden' name='dir' value='".h($dir)."'>
      <input type='hidden' name='mode' value='edit'>
      <input type='hidden' name='origin' value='".h($r["origin"])."'>
      <input type='submit' value='Modifica'>
    </form>
    <form method='post' onsubmit=\"return confirm(\'Cancellare?\');\">
      <input type='hidden' name='tmpkey' value='".h($tmpkey)."'>
      <input type='hidden' name='sort' value='".h($sort)."'>
      <input type='hidden' name='dir' value='".h($dir)."'>
      <input type='hidden' name='mode' value='delete'>
      <input type='hidden' name='origin' value='".h($r["origin"])."'>
      <input type='submit' value='Cancella'>
    </form>
  </td>";
  echo "</tr>";
}
mysqli_free_result($q);

echo "</table>";
echo "</div>";
echo "</div></body></html>";

mysqli_close($con);
?>
