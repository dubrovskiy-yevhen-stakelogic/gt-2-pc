// Compile the actual shared shader source in Chromium and check material pixels.
// Usage: node tests/web_shader_checks.cjs <generated web_shaders.h>
const fs = require('node:fs');
const assert = require('node:assert/strict');
const {chromium} = require('playwright');
(async () => {
  const header = fs.readFileSync(process.argv[2], 'utf8');
  const shaders = [...header.matchAll(/char (vert|frag)\[\] = R"GT2SHADER\(([\s\S]*?)\)GT2SHADER"/g)];
  const sources = Object.fromEntries(shaders.map(m=>[m[1],m[2]]));
  const browser = await chromium.launch({channel:'chrome',headless:true});
  try {
    const page = await browser.newPage();
    const result = await page.evaluate(({vert,frag}) => {
      const canvas = document.createElement('canvas'); canvas.width=canvas.height=16;
      const gl = canvas.getContext('webgl2',{antialias:false,preserveDrawingBuffer:true});
      if(!gl) throw Error('WebGL 2 unavailable');
      const shader=(kind,src)=>{ const s=gl.createShader(kind);gl.shaderSource(s,src);gl.compileShader(s);
        if(!gl.getShaderParameter(s,gl.COMPILE_STATUS)) throw Error(gl.getShaderInfoLog(s));return s; };
      const program=gl.createProgram(); gl.attachShader(program,shader(gl.VERTEX_SHADER,vert));gl.attachShader(program,shader(gl.FRAGMENT_SHADER,frag));
      gl.linkProgram(program);if(!gl.getProgramParameter(program,gl.LINK_STATUS)) throw Error(gl.getProgramInfoLog(program));gl.useProgram(program);
      const texture=(unit,name,internal,w,h,format,type,data,target=gl.TEXTURE_2D)=>{
        gl.activeTexture(gl.TEXTURE0+unit);const t=gl.createTexture();gl.bindTexture(target,t);
        gl.texParameteri(target,gl.TEXTURE_MIN_FILTER,gl.NEAREST);gl.texParameteri(target,gl.TEXTURE_MAG_FILTER,gl.NEAREST);
        if(target===gl.TEXTURE_2D) gl.texImage2D(target,0,internal,w,h,0,format,type,data);
        else gl.texImage3D(target,0,internal,w,h,1,0,format,type,data);
        gl.uniform1i(gl.getUniformLocation(program,name),unit); return t;
      };
      const words=new Uint16Array(1024*512); words[0]=0x001f; words[1]=0x03e0; words[2]=0xfc00;
      const vram=texture(0,'vramImage',gl.R16UI,1024,512,gl.RED_INTEGER,gl.UNSIGNED_SHORT,words);
      const rgba=texture(1,'externalImage',gl.R32UI,4096,1,gl.RED_INTEGER,gl.UNSIGNED_INT,new Uint32Array(4096));
      texture(2,'materialTable',gl.RGBA32UI,256,16,gl.RGBA_INTEGER,gl.UNSIGNED_INT,new Uint32Array(4096*4));
      texture(3,'decodedPages',gl.RGBA8,1,1,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array(4),gl.TEXTURE_2D_ARRAY);
      texture(4,'handAlbedo',gl.RGBA8,1,1,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([255,255,255,255]));
      texture(5,'rearView',gl.RGBA8,1,1,gl.RGBA,gl.UNSIGNED_BYTE,new Uint8Array([200,100,50,255]));
      gl.uniformMatrix4fv(gl.getUniformLocation(program,'pc.mvp'),false,new Float32Array([1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]));
      const set=(name,n)=>gl.uniform1ui(gl.getUniformLocation(program,name),n);
      set('pc.paint',0);set('pc.brakeLit',0);set('pc.stpPass',0);set('pc.options',0);
      gl.uniform4f(gl.getUniformLocation(program,'pc.hudClip'),0,0,0,0);
      const vao=gl.createVertexArray();gl.bindVertexArray(vao);
      const buf=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,buf);gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([-1,-1,.5,3,-1,.5,-1,3,.5]),gl.STATIC_DRAW);
      gl.enableVertexAttribArray(0);gl.vertexAttribPointer(0,3,gl.FLOAT,false,0,0);
      gl.vertexAttrib3f(2,1,1,1);gl.vertexAttribI4ui(3,0,0,0,0);gl.vertexAttribI4ui(4,0,0,0,0);gl.vertexAttrib4f(6,0,0,4,4);
      gl.disable(gl.DITHER);gl.viewport(0,0,16,16);
      const draw=(flags,texel=0,stp=0)=>{
        gl.vertexAttribI4ui(5,flags,0,0,0);gl.vertexAttrib2f(1,texel,0);set('pc.stpPass',stp);
        gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);gl.drawArrays(gl.TRIANGLES,0,3);
        const p=new Uint8Array(4);gl.readPixels(8,8,1,1,gl.RGBA,gl.UNSIGNED_BYTE,p);return [...p];
      };
      const pixels={untextured:draw(0),red15:draw(1|2|512),green15:draw(1|2|512,1),blueStp:draw(1|2|512,2,2),
        stpDiscard:draw(1|2|512,2,1),transparent:draw(1|2|512,3)};
      // 4-bit indexed page and a CLUT separate from its packed image data.
      words[0]=0x0011;words[33]=0x03e0;gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,vram);
      gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,1024,512,gl.RED_INTEGER,gl.UNSIGNED_SHORT,words);
      gl.vertexAttribI4ui(4,32,0,0,0);pixels.clut4=draw(1|2);
      gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,rgba);
      gl.texSubImage2D(gl.TEXTURE_2D,0,0,0,1,1,gl.RED_INTEGER,gl.UNSIGNED_INT,new Uint32Array([0xff332211]));
      gl.vertexAttribI4ui(4,1|(1<<16),0,0,0);pixels.external=draw(1|16);
      pixels.mirror=draw(1<<21);
      return {pixels,error:gl.getError(),renderer:gl.getParameter(gl.RENDERER)};
    }, sources);
    const expected={untextured:[255,255,255,255],red15:[255,0,0,255],green15:[0,255,0,255],blueStp:[0,0,255,255],
      stpDiscard:[0,0,0,255],transparent:[0,0,0,255],clut4:[0,255,0,255],external:[17,34,51,255],mirror:[200,100,50,255]};
    assert.equal(result.error,0);assert.deepEqual(result.pixels,expected);
    console.log(JSON.stringify({status:'PASS',checks:Object.keys(expected).length,...result},null,2));
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
